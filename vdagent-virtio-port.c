/*  vdagent-virtio-port.c virtio port communication code

    Copyright 2010 Red Hat, Inc.

    Red Hat Authors:
    Hans de Goede <hdegoede@redhat.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or   
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of 
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the  
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include "vdagent-virtio-port.h"

struct vdagent_virtio_port_buf {
    uint8_t *buf;
    size_t pos;
    size_t size;
    
    struct vdagent_virtio_port_buf *next;
};

struct vdagent_virtio_port {
    int fd;

    /* Read stuff, single buffer, separate header and data buffer */
    int chunk_header_read;
    int message_header_read;
    VDIChunkHeader chunk_header;
    VDAgentMessage message_header;
    struct vdagent_virtio_port_buf data;

    /* Writes are stored in a linked list of buffers, with both the header
       + data for a single message in 1 buffer. */
    struct vdagent_virtio_port_buf *write_buf;

    /* Callbacks */
    vdagent_virtio_port_read_callback read_callback;
    vdagent_virtio_port_disconnect_callback disconnect_callback;
};

static void vdagent_virtio_port_do_write(struct vdagent_virtio_port **portp);
static void vdagent_virtio_port_do_read(struct vdagent_virtio_port **portp);

struct vdagent_virtio_port *vdagent_virtio_port_create(const char *portname,
    vdagent_virtio_port_read_callback read_callback,
    vdagent_virtio_port_disconnect_callback disconnect_callback)
{
    struct vdagent_virtio_port *port;

    port = calloc(1, sizeof(*port));
    if (!port)
        return 0;

    port->fd = open(portname, O_RDWR);
    if (port->fd == -1) {
        fprintf(stderr, "open %s: %s\n", portname, strerror(errno));
        free(port);
        return NULL;
    }    

    port->read_callback = read_callback;
    port->disconnect_callback = disconnect_callback;

    return port;
}

void vdagent_virtio_port_destroy(struct vdagent_virtio_port **portp)
{
    struct vdagent_virtio_port_buf *wbuf, *next_wbuf;
    struct vdagent_virtio_port *port = *portp;

    if (!port)
        return;

    if (port->disconnect_callback)
        port->disconnect_callback(port);

    wbuf = port->write_buf;
    while (wbuf) {
        next_wbuf = wbuf->next;
        free(wbuf->buf);
        free(wbuf);
        wbuf = next_wbuf;
    }

    free(port->data.buf);

    close(port->fd);
    free(port);
    *portp = NULL;
}

int vdagent_virtio_port_fill_fds(struct vdagent_virtio_port *port,
        fd_set *readfds, fd_set *writefds)
{
    FD_SET(port->fd, readfds);
    if (port->write_buf)
        FD_SET(port->fd, writefds);

    return port->fd + 1;
}

void vdagent_virtio_port_handle_fds(struct vdagent_virtio_port **portp,
        fd_set *readfds, fd_set *writefds)
{
    if (FD_ISSET((*portp)->fd, readfds))
        vdagent_virtio_port_do_read(portp);

    if (*portp && FD_ISSET((*portp)->fd, writefds))
        vdagent_virtio_port_do_write(portp);
}

int vdagent_virtio_port_write(
        struct vdagent_virtio_port *port,
        VDIChunkHeader *chunk_header,
        VDAgentMessage *message_header,
        uint8_t *data)
{
    struct vdagent_virtio_port_buf *wbuf, *new_wbuf;

    if (message_header->size !=
            (chunk_header->size - sizeof(*message_header))) {
        fprintf(stderr, "write: chunk vs message header size mismatch\n");
        return -1;
    }

    new_wbuf = malloc(sizeof(*new_wbuf));
    if (!new_wbuf)
        return -1;

    new_wbuf->pos = 0;
    new_wbuf->size = sizeof(*chunk_header) + sizeof(*message_header) +
                     message_header->size;
    new_wbuf->next = NULL;
    new_wbuf->buf = malloc(new_wbuf->size);
    if (!new_wbuf->buf) {
        free(new_wbuf);
        return -1;
    }

    memcpy(new_wbuf->buf, chunk_header, sizeof(*chunk_header));
    memcpy(new_wbuf->buf + sizeof(*chunk_header), message_header,
           sizeof(*message_header));
    memcpy(new_wbuf->buf + sizeof(*chunk_header) + sizeof(*message_header),
           data, message_header->size);

    if (!port->write_buf) {
        port->write_buf = new_wbuf;
        return 0;
    }

    /* FIXME maybe limit the write_buf stack depth ? */
    wbuf = port->write_buf;
    while (wbuf->next)
        wbuf = wbuf->next;

    wbuf->next = wbuf;

    return 0;
}

static void vdagent_virtio_port_do_read(struct vdagent_virtio_port **portp)
{
    ssize_t n;
    size_t to_read;
    uint8_t *dest;
    int r;
    struct vdagent_virtio_port *port = *portp;

    if (port->chunk_header_read < sizeof(port->chunk_header)) {
        to_read = sizeof(port->chunk_header) - port->chunk_header_read;
        dest = (uint8_t *)&port->chunk_header + port->chunk_header_read;
    } else if (port->message_header_read < sizeof(port->message_header)) {
        to_read = sizeof(port->message_header) - port->message_header_read;
        dest = (uint8_t *)&port->message_header + port->message_header_read;
    } else {
        to_read = port->data.size - port->data.pos;
        dest = port->data.buf + port->data.pos;
    }

    n = read(port->fd, dest, to_read);
    if (n < 0) {
        if (errno == EINTR)
            return;
        perror("reading from vdagent virtio port");
    }
    if (n <= 0) {
        vdagent_virtio_port_destroy(portp);
        return;
    }

    if (port->chunk_header_read < sizeof(port->chunk_header)) {
        port->chunk_header_read += n;
        if (port->chunk_header_read == sizeof(port->chunk_header)) {
            if (port->chunk_header.size < sizeof(port->message_header)) {
                fprintf(stderr, "chunk size < message header size\n");
                vdagent_virtio_port_destroy(portp);
                return;
            }
            port->message_header_read = 0;
        }
    } else if (port->message_header_read < sizeof(port->message_header)) {
        port->message_header_read += n;
        if (port->message_header_read == sizeof(port->message_header)) {
            if (port->message_header.size !=
                    (port->chunk_header.size - sizeof(port->message_header))) {
                fprintf(stderr,
                        "read: chunk vs message header size mismatch\n");
                vdagent_virtio_port_destroy(portp);
                return;
            }
            if (port->message_header.size == 0) {
                if (port->read_callback) {
                    r = port->read_callback(port, &port->chunk_header,
                                            &port->message_header, NULL);
                    if (r == -1) {
                        vdagent_virtio_port_destroy(portp);
                        return;
                    }
                }
                port->chunk_header_read = 0;
                port->message_header_read = 0;
            } else {
                port->data.pos = 0;
                port->data.size = port->message_header.size;
                port->data.buf = malloc(port->data.size);
                if (!port->data.buf) {
                    fprintf(stderr, "out of memory, disportecting client\n");
                    vdagent_virtio_port_destroy(portp);
                    return;
                }
            }
        }
    } else {
        port->data.pos += n;
        if (port->data.pos == port->data.size) {
            if (port->read_callback) {
                r = port->read_callback(port, &port->chunk_header,
                                        &port->message_header, port->data.buf);
                if (r == -1) {
                    vdagent_virtio_port_destroy(portp);
                    return;
                }
            }
            free(port->data.buf);
            port->chunk_header_read = 0;
            port->message_header_read = 0;
            memset(&port->data, 0, sizeof(port->data));
        }
    }
}

static void vdagent_virtio_port_do_write(struct vdagent_virtio_port **portp)
{
    ssize_t n;
    size_t to_write;
    struct vdagent_virtio_port *port = *portp;

    struct vdagent_virtio_port_buf* wbuf = port->write_buf;
    if (!wbuf) {
        fprintf(stderr,
                "do_write called on a port without a write buf ?!\n");
        return;
    }

    to_write = wbuf->size - wbuf->pos;
    n = write(port->fd, wbuf->buf + wbuf->pos, to_write);
    if (n < 0) {
        if (errno == EINTR)
            return;
        perror("writing to vdagent virtio port");
        vdagent_virtio_port_destroy(portp);
        return;
    }

    wbuf->pos += n;
    if (wbuf->pos == wbuf->size) {
        port->write_buf = wbuf->next;
        free(wbuf->buf);
        free(wbuf);
    }
}