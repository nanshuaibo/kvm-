/*
 * QEMU live migration via socket
 *
 * Copyright Red Hat, Inc. 2009-2016
 *
 * Authors:
 *  Chris Lalancette <clalance@redhat.com>
 *  Daniel P. Berrange <berrange@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"

#include "qemu/error-report.h"
#include "qapi/error.h"
#include "channel.h"
#include "socket.h"
#include "migration.h"
#include "qemu-file.h"
#include "io/channel-socket.h"
#include "io/net-listener.h"
#include "trace.h"
#include "postcopy-ram.h"

struct SocketOutgoingArgs {
    SocketAddress *saddr;
} outgoing_args;

void socket_send_channel_create(QIOTaskFunc f, void *data)
{
    QIOChannelSocket *sioc = qio_channel_socket_new();
    qio_channel_socket_connect_async(sioc, outgoing_args.saddr,
                                     f, data, NULL, NULL);
}

QIOChannel *socket_send_channel_create_sync(Error **errp)
{
    QIOChannelSocket *sioc = qio_channel_socket_new();

    if (!outgoing_args.saddr) {
        object_unref(OBJECT(sioc));
        error_setg(errp, "Initial sock address not set!");
        return NULL;
    }

    if (qio_channel_socket_connect_sync(sioc, outgoing_args.saddr, errp) < 0) {
        object_unref(OBJECT(sioc));
        return NULL;
    }

    return QIO_CHANNEL(sioc);
}

int socket_send_channel_destroy(QIOChannel *send)
{
    /* Remove channel */
    object_unref(OBJECT(send));
    if (outgoing_args.saddr) {
        qapi_free_SocketAddress(outgoing_args.saddr);
        outgoing_args.saddr = NULL;
    }
    return 0;
}

struct SocketConnectData {
    MigrationState *s; //迁移状态信息
    char *hostname; //迁移母机
};

static void socket_connect_data_free(void *opaque)
{
    struct SocketConnectData *data = opaque;
    if (!data) {
        return;
    }
    g_free(data->hostname);
    g_free(data);
}

static void socket_outgoing_migration(QIOTask *task,
                                      gpointer opaque)
{
    struct SocketConnectData *data = opaque;
    QIOChannel *sioc = QIO_CHANNEL(qio_task_get_source(task));
    Error *err = NULL;

    if (qio_task_propagate_error(task, &err)) {
        trace_migration_socket_outgoing_error(error_get_pretty(err));
           goto out;
    }

    trace_migration_socket_outgoing_connected(data->hostname);

    // 如果启用了零拷贝发送功能，但检测不到主机内核支持该功能，则设置错误
    if (migrate_use_zero_copy_send() &&
        !qio_channel_has_feature(sioc, QIO_CHANNEL_FEATURE_WRITE_ZERO_COPY)) {
        error_setg(&err, "Zero copy send feature not detected in host kernel");
    }

out:

    //建立迁移通道
    migration_channel_connect(data->s, sioc, data->hostname, err);
    object_unref(OBJECT(sioc));
}

static void
socket_start_outgoing_migration_internal(MigrationState *s,
                                         SocketAddress *saddr,
                                         Error **errp)
{
    QIOChannelSocket *sioc = qio_channel_socket_new();
    struct SocketConnectData *data = g_new0(struct SocketConnectData, 1);

    data->s = s;

    /* in case previous migration leaked it */
    qapi_free_SocketAddress(outgoing_args.saddr);
    outgoing_args.saddr = saddr;

    if (saddr->type == SOCKET_ADDRESS_TYPE_INET) {
        data->hostname = g_strdup(saddr->u.inet.host);
    }

    qio_channel_set_name(QIO_CHANNEL(sioc), "migration-socket-outgoing");
    qio_channel_socket_connect_async(sioc,
                                     saddr,
                                     socket_outgoing_migration, //回调函数，当迁移套接字创建成功时调用
                                     socket_connect_data_free,
                                     NULL);
}

void socket_start_outgoing_migration(MigrationState *s,
                                     const char *str,
                                     Error **errp)
{
    Error *err = NULL;
    SocketAddress *saddr = socket_parse(str, &err); //解析传入的uri，建立连接，获取SocketAddress对象
    if (!err) {
        socket_start_outgoing_migration_internal(s, saddr, &err);//开始传输迁移数据
    }
    error_propagate(errp, err);
}

static void socket_accept_incoming_migration(QIONetListener *listener,
                                             QIOChannelSocket *cioc,
                                             gpointer opaque)
{
    trace_migration_socket_incoming_accepted();

    if (migration_has_all_channels()) {
        error_report("%s: Extra incoming migration connection; ignoring",
                     __func__);
        return;
    }

    // 设置通道的名称
    qio_channel_set_name(QIO_CHANNEL(cioc), "migration-socket-incoming");
    // 处理传入的迁移通道
    migration_channel_process_incoming(QIO_CHANNEL(cioc));
}

static void
socket_incoming_migration_end(void *opaque)
{
    QIONetListener *listener = opaque;

    qio_net_listener_disconnect(listener);
    object_unref(OBJECT(listener));
}

static void
socket_start_incoming_migration_internal(SocketAddress *saddr,
                                         Error **errp)
{
    QIONetListener *listener = qio_net_listener_new();
    MigrationIncomingState *mis = migration_incoming_get_current();
    size_t i;
    int num = 1;

    qio_net_listener_set_name(listener, "migration-socket-listener");

    //根据迁移策略设置监听器额通道数
    if (migrate_use_multifd()) {
        num = migrate_multifd_channels();
    } else if (migrate_postcopy_preempt()) {
        num = RAM_CHANNEL_MAX;
    }

    if (qio_net_listener_open_sync(listener, saddr, num,  errp) < 0) {
        object_unref(OBJECT(listener));
        return;
    }

    mis->transport_data = listener;
    //设置迁移状态的传输清理函数
    mis->transport_cleanup = socket_incoming_migration_end;

    qio_net_listener_set_client_func_full(listener,
                                          socket_accept_incoming_migration, //处理函数
                                          NULL, NULL,
                                          g_main_context_get_thread_default());
    // 遍历监听器的所有连接
    for (i = 0; i < listener->nsioc; i++)  {
        SocketAddress *address =
            qio_channel_socket_get_local_address(listener->sioc[i], errp);
        if (!address) {
            return;
        }
        migrate_add_address(address);
        qapi_free_SocketAddress(address);
    }
}

void socket_start_incoming_migration(const char *str, Error **errp)
{
    Error *err = NULL;
    SocketAddress *saddr = socket_parse(str, &err);// 解析传入的uri，建立连接，获取SocketAddress对象
    if (!err) {
        socket_start_incoming_migration_internal(saddr, &err); //开始接受迁移数据
    }
    qapi_free_SocketAddress(saddr);
    error_propagate(errp, err);
}
