#include <bsd/bsd.h>
#include <bsd/console.h>
#include <bsd/errno.h>
#include <bsd/proc.h>
#include <bsd/vfs.h>
#include <io.h>
#include <klog.h>
#include <string.h>
#include <types.h>

#define CONSOLE_RX_CAPACITY 256

#define CONSOLE_CTRL_C   0x03
#define CONSOLE_BACKSPACE 0x08
#define CONSOLE_DELETE   0x7F

static vnode_t console_node;
static uint8_t console_rx[CONSOLE_RX_CAPACITY];
static size_t console_rx_head;
static size_t console_rx_tail;
static size_t console_rx_count;
static size_t console_line_len;
static size_t console_lines;
static bool console_last_was_cr;
static int32_t console_foreground_pid;
static int32_t console_fg_pgid;

static sys_status_t console_read(vnode_t *vnode, void *buf, size_t len,
                                 uint64_t offset, size_t *done);
static sys_status_t console_write(vnode_t *vnode, const void *buf, size_t len,
                                  uint64_t offset, size_t *done);

static vnode_ops_t console_ops = {
    .read = console_read,
    .write = console_write,
    .lookup = NULL,
    .readdir = NULL,
    .close = NULL,
    .release = NULL,
};

static void console_emit(char c)
{
    if (c == '\n') {
        serial_putc('\r');
        serial_putc('\n');
        return;
    }
    serial_putc(c);
}

static void console_emit_str(const char *text)
{
    while (*text != '\0') {
        console_emit(*text++);
    }
}

/* Caller must hold the console lock (interrupts disabled). */
static bool console_push(uint8_t byte)
{
    if (console_rx_count >= CONSOLE_RX_CAPACITY) {
        return false;
    }

    console_rx[console_rx_head] = byte;
    console_rx_head = (console_rx_head + 1) % CONSOLE_RX_CAPACITY;
    console_rx_count++;
    return true;
}

/* Caller must hold the console lock (interrupts disabled). */
static uint8_t console_pop(void)
{
    uint8_t byte = console_rx[console_rx_tail];

    console_rx_tail = (console_rx_tail + 1) % CONSOLE_RX_CAPACITY;
    console_rx_count--;
    return byte;
}

static void console_handle_input(uint8_t byte)
{
    if (byte == '\n' && console_last_was_cr) {
        console_last_was_cr = false;
        return;
    }

    bool was_cr = byte == '\r';
    console_last_was_cr = was_cr;

    if (byte == CONSOLE_CTRL_C) {
        uint64_t flags = irq_save();
        while (console_line_len > 0 && console_rx_count > 0) {
            console_rx_head = (console_rx_head + CONSOLE_RX_CAPACITY - 1) %
                              CONSOLE_RX_CAPACITY;
            console_rx_count--;
            console_line_len--;
        }
        int32_t fg_pgid = console_fg_pgid;
        int32_t foreground = console_foreground_pid;
        irq_restore(flags);

        console_emit_str("^C\n");

        /* Prefer process group delivery if fg_pgid is set. */
        if (fg_pgid > 0) {
            (void)bsd_signal_send_group(fg_pgid, BSD_SIGINT);
        } else if (foreground > 0) {
            (void)bsd_signal_send(foreground, BSD_SIGINT);
        }
        return;
    }

    if (was_cr || byte == '\n') {
        uint64_t flags = irq_save();
        bool stored = console_push((uint8_t)'\n');
        if (stored) {
            console_lines++;
            console_line_len = 0;
        }
        irq_restore(flags);

        if (stored) {
            console_emit('\n');
        }
        return;
    }

    if (byte == CONSOLE_BACKSPACE || byte == CONSOLE_DELETE) {
        uint64_t flags = irq_save();
        bool erased = false;

        if (console_line_len > 0 && console_rx_count > 0) {
            console_rx_head = (console_rx_head + CONSOLE_RX_CAPACITY - 1) %
                              CONSOLE_RX_CAPACITY;
            console_rx_count--;
            console_line_len--;
            erased = true;
        }
        irq_restore(flags);

        if (erased) {
            serial_putc('\b');
            serial_putc(' ');
            serial_putc('\b');
        }
        return;
    }

    if (byte == '\t' || (byte >= 0x20 && byte <= 0x7E)) {
        uint64_t flags = irq_save();
        bool stored = console_push(byte);
        if (stored) {
            console_line_len++;
        }
        irq_restore(flags);

        if (stored) {
            console_emit((char)byte);
        }
    }
}

static sys_status_t console_read(vnode_t *vnode, void *buf, size_t len,
                                 uint64_t offset, size_t *done)
{
    UNUSED(vnode);
    UNUSED(offset);

    if (done == NULL || (buf == NULL && len != 0)) {
        return BSD_ERR(BSD_EINVAL);
    }

    *done = 0;
    if (len == 0) {
        return SYS_OK;
    }

    uint8_t *out = (uint8_t *)buf;

    for (;;) {
        bsd_proc_t *proc = bsd_proc_current();

        if (proc != NULL && bsd_proc_has_pending(proc)) {
            *done = 0;
            return BSD_ERR(BSD_EINTR);
        }

        uint64_t flags = irq_save();

        if (console_lines > 0) {
            size_t copied = 0;

            while (copied < len && console_rx_count > 0) {
                uint8_t byte = console_pop();
                out[copied++] = byte;

                if (byte == '\n') {
                    if (console_lines > 0) {
                        console_lines--;
                    }
                    break;
                }
            }

            irq_restore(flags);
            *done = copied;
            return SYS_OK;
        }

        irq_restore(flags);

        int received = serial_getc();
        if (received >= 0) {
            console_handle_input((uint8_t)received);
            continue;
        }

        thread_sleep_ms(1);
    }
}

static sys_status_t console_write(vnode_t *vnode, const void *buf, size_t len,
                                  uint64_t offset, size_t *done)
{
    UNUSED(vnode);
    UNUSED(offset);

    if (done == NULL || (buf == NULL && len != 0)) {
        return BSD_ERR(BSD_EINVAL);
    }

    const char *text = (const char *)buf;

    for (size_t i = 0; i < len; i++) {
        console_emit(text[i]);
    }

    *done = len;
    return SYS_OK;
}

void console_init(void)
{
    memset(&console_node, 0, sizeof(console_node));
    memset(console_rx, 0, sizeof(console_rx));

    console_rx_head = 0;
    console_rx_tail = 0;
    console_rx_count = 0;
    console_line_len = 0;
    console_lines = 0;
    console_last_was_cr = false;
    console_foreground_pid = 0;
    console_fg_pgid = 0;

    console_node.type = VNODE_CHR;
    console_node.mode = BSD_S_IFCHR | 0620U;
    console_node.size = 0;
    console_node.ino = 3;
    console_node.ref_count = 1;
    console_node.ops = &console_ops;
    console_node.fs_data = NULL;
    console_node.parent = NULL;
    console_node.name = "console";

    klog(KLOG_INFO, "bsd: /dev/console bound to COM1 (115200 8N1)");
}

vnode_t *console_vnode(void)
{
    return &console_node;
}

void console_set_foreground(int32_t pid)
{
    uint64_t flags = irq_save();
    console_foreground_pid = pid;
    irq_restore(flags);
}

int32_t console_foreground(void)
{
    uint64_t flags = irq_save();
    int32_t pid = console_foreground_pid;
    irq_restore(flags);
    return pid;
}

void console_set_foreground_pgid(int32_t pgid)
{
    uint64_t flags = irq_save();
    console_fg_pgid = pgid;
    irq_restore(flags);
}

int32_t console_foreground_pgid(void)
{
    uint64_t flags = irq_save();
    int32_t pgid = console_fg_pgid;
    irq_restore(flags);
    return pgid;
}
