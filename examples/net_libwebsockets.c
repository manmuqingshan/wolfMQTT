/* net_libwebsockets.c
 *
 * Copyright (C) 2006-2023 wolfSSL Inc.
 *
 * This file is part of wolfMQTT.
 *
 * wolfMQTT is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * wolfMQTT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA
 */

/* Include the autoconf generated config.h */
#ifdef HAVE_CONFIG_H
    #include <config.h>
#endif

#include "wolfmqtt/mqtt_client.h"
#include "examples/mqttnet.h"
#include "examples/mqttexample.h"
#include "examples/net_libwebsockets.h"

#ifdef ENABLE_MQTT_WEBSOCKET

#include <libwebsockets.h>

/* Network context for libwebsockets */
typedef struct _LibwebsockContext {
    struct lws_context *context;
    struct lws *wsi;
    char* host;
    int port;
    int status;
    void* heap;
    /* Buffer for received data */
    unsigned char rx_buffer[1024];
    size_t rx_len;
} LibwebsockContext;

/* Callback for libwebsockets events */
static int callback_mqtt(struct lws *wsi, enum lws_callback_reasons reason,
    void *user, void *in, size_t len)
{
    LibwebsockContext *net = (LibwebsockContext*)user;
    
    (void)wsi;
    (void)in;
    (void)len;
    
    /* Only handle the events we care about */
    if (reason == LWS_CALLBACK_CLIENT_ESTABLISHED) {
        net->status = 1;
    }
    else if (reason == LWS_CALLBACK_CLIENT_CONNECTION_ERROR) {
        net->status = -1;
    }
    else if (reason == LWS_CALLBACK_CLOSED) {
        net->status = 0;
    }
    else if (reason == LWS_CALLBACK_CLIENT_RECEIVE) {
        /* Store received data */
        if (in && len > 0 && len <= sizeof(net->rx_buffer)) {
            memcpy(net->rx_buffer, in, len);
            net->rx_len = len;
        }
    }
    
    return 0;
}

static const struct lws_protocols protocols[] = {
    {
        "mqtt",
        callback_mqtt,
        sizeof(LibwebsockContext),
        0,
        0, /* id */
        NULL, /* user */
        0 /* tx_packet_size */
    },
    { NULL, NULL, 0, 0, 0, NULL, 0 }
};

int NetWebsocket_Connect(void *context, const char* host, word16 port,
    int timeout_ms)
{
    SocketContext *sock = (SocketContext*)context;
    LibwebsockContext *net;
    struct lws_client_connect_info conn_info;
    struct lws_context_creation_info info;
    int wait_ms = timeout_ms;
    
    if (sock == NULL || host == NULL) {
        return MQTT_CODE_ERROR_BAD_ARG;
    }
    
    /* Create libwebsocket context */
    net = (LibwebsockContext*)WOLFMQTT_MALLOC(sizeof(LibwebsockContext));
    if (net == NULL) {
        return MQTT_CODE_ERROR_MEMORY;
    }
    XMEMSET(net, 0, sizeof(LibwebsockContext));
    
    /* Store in socket context */
    sock->websocket_ctx = net;
    
    /* Initialize context */
    XMEMSET(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    
    net->context = lws_create_context(&info);
    if (net->context == NULL) {
        WOLFMQTT_FREE(net);
        sock->websocket_ctx = NULL;
        return MQTT_CODE_ERROR_NETWORK;
    }
    
    XMEMSET(&conn_info, 0, sizeof(conn_info));
    conn_info.context = net->context;
    conn_info.address = host;
    conn_info.port = port;
    conn_info.path = "/mqtt";
    conn_info.host = host;
    conn_info.protocol = "mqtt";
    conn_info.pwsi = &net->wsi;
    
    net->wsi = lws_client_connect_via_info(&conn_info);
    if (net->wsi == NULL) {
        lws_context_destroy(net->context);
        WOLFMQTT_FREE(net);
        sock->websocket_ctx = NULL;
        return MQTT_CODE_ERROR_NETWORK;
    }
    
    /* Wait for connection */
    while (wait_ms > 0 && net->status == 0) {
        lws_service(net->context, 100);
        wait_ms -= 100;
    }
    
    return (net->status > 0) ? MQTT_CODE_SUCCESS : MQTT_CODE_ERROR_NETWORK;
}

int NetWebsocket_Write(void *context, const byte* buf, int buf_len,
    int timeout_ms)
{
    SocketContext *sock = (SocketContext*)context;
    LibwebsockContext *net;
    unsigned char *ws_buf;
    int ret;
    
    (void)timeout_ms;
    
    if (sock == NULL || buf == NULL || buf_len <= 0) {
        return MQTT_CODE_ERROR_BAD_ARG;
    }
    
    net = (LibwebsockContext*)sock->websocket_ctx;
    if (net == NULL) {
        return MQTT_CODE_ERROR_BAD_ARG;
    }
    
    /* Add LWS_PRE bytes for libwebsockets header */
    ws_buf = (unsigned char*)WOLFMQTT_MALLOC(LWS_PRE + buf_len);
    if (ws_buf == NULL) {
        return MQTT_CODE_ERROR_MEMORY;
    }
    
    XMEMCPY(ws_buf + LWS_PRE, buf, buf_len);
    
    ret = lws_write(net->wsi, ws_buf + LWS_PRE, buf_len, LWS_WRITE_BINARY);
    
    WOLFMQTT_FREE(ws_buf);
    
    return (ret > 0) ? ret : MQTT_CODE_ERROR_NETWORK;
}

int NetWebsocket_Read(void *context, byte* buf, int buf_len,
    int timeout_ms)
{
    SocketContext *sock = (SocketContext*)context;
    LibwebsockContext *net;
    int ret = 0;
    int wait_ms = timeout_ms;
    
    (void)timeout_ms;
    
    if (sock == NULL || buf == NULL || buf_len <= 0) {
        return MQTT_CODE_ERROR_BAD_ARG;
    }
    
    net = (LibwebsockContext*)sock->websocket_ctx;
    if (net == NULL) {
        return MQTT_CODE_ERROR_BAD_ARG;
    }
    
    /* Check if we already have data */
    if (net->rx_len > 0) {
        ret = (net->rx_len <= (size_t)buf_len) ? (int)net->rx_len : buf_len;
        XMEMCPY(buf, net->rx_buffer, ret);
        
        /* If we didn't consume all data, move remaining data to beginning of buffer */
        if (ret < (int)net->rx_len) {
            XMEMMOVE(net->rx_buffer, net->rx_buffer + ret, net->rx_len - ret);
            net->rx_len -= ret;
        } else {
            net->rx_len = 0;
        }
        
        return ret;
    }
    
    /* Wait for data */
    while (wait_ms > 0 && net->rx_len == 0) {
        lws_service(net->context, 100);
        wait_ms -= 100;
        
        if (net->rx_len > 0) {
            ret = (net->rx_len <= (size_t)buf_len) ? (int)net->rx_len : buf_len;
            XMEMCPY(buf, net->rx_buffer, ret);
            
            /* If we didn't consume all data, move remaining data to beginning of buffer */
            if (ret < (int)net->rx_len) {
                XMEMMOVE(net->rx_buffer, net->rx_buffer + ret, net->rx_len - ret);
                net->rx_len -= ret;
            } else {
                net->rx_len = 0;
            }
            
            return ret;
        }
    }
    
    return (wait_ms <= 0) ? MQTT_CODE_ERROR_TIMEOUT : MQTT_CODE_ERROR_NETWORK;
}

int NetWebsocket_Disconnect(void *context)
{
    SocketContext *sock = (SocketContext*)context;
    LibwebsockContext *net;
    
    if (sock == NULL) {
        return MQTT_CODE_ERROR_BAD_ARG;
    }
    
    net = (LibwebsockContext*)sock->websocket_ctx;
    if (net == NULL) {
        return MQTT_CODE_SUCCESS; /* Already disconnected */
    }
    
    if (net->wsi) {
        lws_close_reason(net->wsi, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
        net->wsi = NULL;
    }
    
    if (net->context) {
        lws_context_destroy(net->context);
        net->context = NULL;
    }
    
    WOLFMQTT_FREE(net);
    sock->websocket_ctx = NULL;
    
    return MQTT_CODE_SUCCESS;
}

#endif /* ENABLE_MQTT_WEBSOCKET */ 