/**
 * uart_test.c — Linux UART Interface using termios API
 *
 * LFX Mentorship Challenge: RISC-V ACT Framework Enablement
 * and M-Mode Firmware Validation on Hardware Board
 *
 * Purpose:
 *   This program opens, configures, and communicates over a UART serial
 *   interface on Linux. It is designed to interface with RISC-V development
 *   boards where UART is the primary channel for firmware debug output,
 *   ACT (Architecture Compatibility Test) result streaming, and M-Mode
 *   firmware validation responses.
 *
 * Features:
 *   - Configurable baud rate, data bits, parity, stop bits via CLI args
 *   - Non-blocking receive using both select() and poll() (compile-time switch)
 *   - Loopback self-test mode (--loopback) to verify TX == RX
 *   - Hex + ASCII dump of all received bytes (firmware-debug friendly)
 *   - Graceful terminal state restoration via atexit()
 *   - Detailed error messages with actionable fix suggestions
 *
 * Build:
 *   make              (default, uses select())
 *   make USE_POLL=1   (uses poll() instead)
 *
 * Usage:
 *   ./uart_test <device> <baud_rate> [--loopback]
 *   ./uart_test /dev/ttyUSB0 115200
 *   ./uart_test /dev/ttyUSB0 115200 --loopback
 *   ./uart_test /dev/pts/3   9600
 *
 * Author: Ammarah Wakeel
 * License: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <sys/stat.h>

/* Compile-time switch: define USE_POLL to use poll() instead of select() */
#ifdef USE_POLL
#  include <poll.h>
#else
#  include <sys/select.h>
#  include <sys/time.h>
#endif

/* ─────────────────────────── Constants ─────────────────────────────────── */

#define RX_TIMEOUT_SEC      3       /* Seconds to wait for incoming data     */
#define RX_BUF_SIZE         256     /* Receive buffer size in bytes          */
#define TEST_MESSAGE        "UART_TEST:HELLO_RISCV_ACT_Framework_mentors\n"
#define HEX_DUMP_WIDTH      16      /* Bytes per line in hex dump            */

/* ─────────────────────────── Globals ───────────────────────────────────── */

static int          g_uart_fd   = -1;   /* Global FD so atexit can close it  */
static struct termios g_orig_termios;   /* Saved original terminal settings  */
static bool         g_restored  = false;/* Guard against double-restore      */

/* ─────────────────────────── Baud Rate Table ───────────────────────────── */

/**
 * baud_entry — maps a human-readable integer baud rate to the termios
 * speed_t constant. This covers every rate a RISC-V board is likely to use.
 */
typedef struct {
    int      rate;   /* e.g. 115200          */
    speed_t  code;   /* e.g. B115200         */
} BaudEntry;

static const BaudEntry BAUD_TABLE[] = {
    {    1200,    B1200 },
    {    2400,    B2400 },
    {    4800,    B4800 },
    {    9600,    B9600 },
    {   19200,   B19200 },
    {   38400,   B38400 },
    {   57600,   B57600 },
    {  115200,  B115200 },
    {  230400,  B230400 },
    {  460800,  B460800 },
    {  921600,  B921600 },
    { 0, 0 } /* sentinel */
};

/**
 * baud_to_speed() — look up the termios speed_t for a given integer baud rate.
 *
 * @param baud  Integer baud rate (e.g. 115200)
 * @return      Matching speed_t constant, or B0 if not found
 */
static speed_t baud_to_speed(int baud)
{
    for (int i = 0; BAUD_TABLE[i].rate != 0; i++) {
        if (BAUD_TABLE[i].rate == baud)
            return BAUD_TABLE[i].code;
    }
    return B0; /* B0 signals "not found" */
}

/* ─────────────────────────── Cleanup / Signal Handling ─────────────────── */

/**
 * restore_terminal() — restores the original termios settings and closes
 * the UART file descriptor. Registered with atexit() so it runs even on
 * abnormal exits (Ctrl+C, assert, exit()).
 *
 * On RISC-V hardware, failing to restore settings can leave the port in a
 * broken state for the next user / test run.
 */
static void restore_terminal(void)
{
    if (g_restored) return;
    g_restored = true;

    if (g_uart_fd >= 0) {
        /* Flush any pending output before restoring */
        tcdrain(g_uart_fd);
        /* Restore the settings we saved before reconfiguring */
        tcsetattr(g_uart_fd, TCSANOW, &g_orig_termios);
        close(g_uart_fd);
        g_uart_fd = -1;
        printf("\n[uart_test] Terminal restored and port closed.\n");
    }
}

/**
 * sigint_handler() — catches SIGINT (Ctrl+C) so the user gets a clean exit
 * instead of an abrupt kill. atexit() will handle the actual cleanup.
 */
static void sigint_handler(int sig)
{
    (void)sig; /* suppress unused-parameter warning */
    printf("\n[uart_test] Caught SIGINT — exiting cleanly...\n");
    exit(EXIT_SUCCESS); /* triggers atexit → restore_terminal */
}

/* ─────────────────────────── UART Open ─────────────────────────────────── */

/**
 * open_uart() — opens the serial device with O_RDWR.
 *
 * Flags used:
 *   O_RDWR    — we need both read and write access
 *   O_NOCTTY  — don't make this port our controlling terminal
 *   O_NDELAY  — open in non-blocking mode initially (we switch later)
 *
 * @param device  Path to serial device, e.g. "/dev/ttyUSB0"
 * @return        File descriptor on success, -1 on failure
 */
static int open_uart(const char *device)
{
    /* ── Pre-flight checks ───────────────────────────────────────────────── */

    struct stat st;
    if (stat(device, &st) < 0) {
        /* Device path does not exist at all */
        fprintf(stderr,
            "[ERROR] Device '%s' not found: %s\n"
            "  → Is the USB-serial adapter plugged in?\n"
            "  → Run: ls /dev/tty* to list available ports\n",
            device, strerror(errno));
        return -1;
    }

    if (!S_ISCHR(st.st_mode)) {
        /* Path exists but is not a character device */
        fprintf(stderr,
            "[ERROR] '%s' is not a character device.\n"
            "  → Expected something like /dev/ttyUSB0 or /dev/ttyS0\n",
            device);
        return -1;
    }

    /* ── Open the device ─────────────────────────────────────────────────── */

    int fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM) {
            /* This is the #1 error new users hit — give a clear fix */
            fprintf(stderr,
                "[ERROR] Permission denied on '%s'.\n"
                "  → Fix: sudo usermod -aG dialout $USER  (then log out/in)\n"
                "  → Or:  sudo chmod a+rw %s  (temporary)\n"
                "  → Or:  sudo ./uart_test %s <baud>  (quick test)\n",
                device, device, device);
        } else {
            fprintf(stderr, "[ERROR] Cannot open '%s': %s\n",
                device, strerror(errno));
        }
        return -1;
    }

    /* Switch back to blocking mode after open */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        fprintf(stderr, "[ERROR] fcntl failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    printf("[uart_test] Opened '%s' (fd=%d)\n", device, fd);
    return fd;
}

/* ─────────────────────────── UART Configure ────────────────────────────── */

/**
 * configure_uart() — applies 8N1 (or similar) settings to the open port.
 *
 * Configuration applied:
 *   - Baud rate    : caller-specified (both input and output)
 *   - Data bits    : 8  (CS8)
 *   - Parity       : None (PARENB cleared)
 *   - Stop bits    : 1  (CSTOPB cleared)
 *   - Flow control : None (CRTSCTS cleared, XON/XOFF cleared)
 *   - Mode         : Raw (cfmakeraw equivalent flags)
 *
 * We save the original settings first so restore_terminal() can put them back.
 *
 * @param fd    Open file descriptor for the UART device
 * @param baud  Desired baud rate as an integer (e.g. 115200)
 * @return      0 on success, -1 on failure
 */
static int configure_uart(int fd, int baud)
{
    /* ── Save original settings ──────────────────────────────────────────── */
    if (tcgetattr(fd, &g_orig_termios) < 0) {
        fprintf(stderr, "[ERROR] tcgetattr failed: %s\n", strerror(errno));
        return -1;
    }

    /* ── Look up termios speed constant ─────────────────────────────────── */
    speed_t speed = baud_to_speed(baud);
    if (speed == B0) {
        fprintf(stderr,
            "[ERROR] Unsupported baud rate: %d\n"
            "  → Supported: 1200 2400 4800 9600 19200 38400\n"
            "               57600 115200 230400 460800 921600\n",
            baud);
        return -1;
    }

    /* ── Build new settings from scratch ─────────────────────────────────── */
    struct termios tty;
    memset(&tty, 0, sizeof(tty));

    /*
     * Input flags (c_iflag) — disable all special input processing.
     *   IGNBRK  : ignore BREAK condition
     *   BRKINT  : don't send SIGINT on BREAK
     *   PARMRK  : don't mark parity/framing errors
     *   ISTRIP  : don't strip 8th bit
     *   INLCR   : don't translate NL→CR
     *   IGNCR   : don't ignore CR
     *   ICRNL   : don't translate CR→NL  ← critical for binary protocols
     *   IXON    : disable XON/XOFF flow control (software)
     */
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP |
                     INLCR  | IGNCR  | ICRNL  | IXON);

    /*
     * Output flags (c_oflag) — disable output post-processing.
     *   OPOST : no implementation-defined output processing
     */
    tty.c_oflag &= ~OPOST;

    /*
     * Local flags (c_lflag) — raw mode: no echo, no signals, no canonical.
     *   ECHO   : don't echo input characters
     *   ECHONL : don't echo NL even if ECHO is off
     *   ICANON : disable canonical (line-buffered) mode → byte-by-byte
     *   ISIG   : don't generate signals from special chars (Ctrl+C etc.)
     *   IEXTEN : disable implementation-defined extensions
     */
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

    /*
     * Control flags (c_cflag) — hardware line settings.
     *   CS8     : 8 data bits
     *   CREAD   : enable receiver
     *   CLOCAL  : ignore modem control lines (no carrier detect needed)
     *   Clear PARENB  → no parity
     *   Clear CSTOPB  → 1 stop bit
     *   Clear CRTSCTS → no hardware flow control
     */
    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    tty.c_cflag |=  (CS8 | CREAD | CLOCAL);

    /*
     * VMIN / VTIME — control blocking behaviour of read().
     *   VMIN=0, VTIME=0 → purely non-blocking (read returns immediately)
     *   We use select()/poll() for the timeout, so non-blocking read is fine.
     */
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    /* Set baud rate (input and output must match for most UART hardware) */
    if (cfsetispeed(&tty, speed) < 0 || cfsetospeed(&tty, speed) < 0) {
        fprintf(stderr, "[ERROR] cfsetspeed failed: %s\n", strerror(errno));
        return -1;
    }

    /*
     * Apply settings immediately (TCSANOW).
     * TCSAFLUSH would also discard pending I/O — useful on noisy lines,
     * but TCSANOW is sufficient for a fresh open.
     */
    if (tcsetattr(fd, TCSANOW, &tty) < 0) {
        fprintf(stderr, "[ERROR] tcsetattr failed: %s\n", strerror(errno));
        return -1;
    }

    /* Flush any stale data in kernel TX/RX buffers */
    tcflush(fd, TCIOFLUSH);

    printf("[uart_test] Port configured: %d baud, 8N1, no flow control\n", baud);
    return 0;
}

/* ─────────────────────────── Transmit ──────────────────────────────────── */

/**
 * transmit() — writes a null-terminated message to the UART.
 *
 * Uses a loop to handle partial writes (write() may return fewer bytes
 * than requested if the kernel TX buffer is temporarily full).
 *
 * @param fd   Open, configured UART file descriptor
 * @param msg  Null-terminated string to transmit
 * @return     0 on success, -1 on failure
 */
static int transmit(int fd, const char *msg)
{
    size_t total  = strlen(msg);
    size_t sent   = 0;

    printf("[uart_test] TX (%zu bytes): %s", total, msg);

    while (sent < total) {
        ssize_t n = write(fd, msg + sent, total - sent);
        if (n < 0) {
            if (errno == EINTR) continue; /* interrupted by signal, retry */
            fprintf(stderr, "[ERROR] write() failed: %s\n", strerror(errno));
            return -1;
        }
        sent += (size_t)n;
    }

    /* Wait until all bytes have physically left the TX FIFO */
    tcdrain(fd);

    printf("[uart_test] TX complete (%zu bytes sent)\n", sent);
    return 0;
}

/* ─────────────────────────── Hex Dump ──────────────────────────────────── */

/**
 * hex_dump() — prints received bytes as both hex and printable ASCII.
 *
 * This is invaluable when talking to RISC-V firmware: binary responses,
 * escape sequences, and non-printable control bytes are all visible.
 *
 * Example output for "Hello\n":
 *   0000  48 65 6c 6c 6f 0a                          Hello.
 *
 * @param buf  Pointer to data buffer
 * @param len  Number of bytes in buffer
 */
static void hex_dump(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i += HEX_DUMP_WIDTH) {
        /* Offset column */
        printf("  %04zx  ", i);

        /* Hex columns */
        for (size_t j = 0; j < HEX_DUMP_WIDTH; j++) {
            if (i + j < len)
                printf("%02x ", buf[i + j]);
            else
                printf("   "); /* pad incomplete last row */
        }

        /* ASCII column — replace non-printable bytes with '.' */
        printf("  ");
        for (size_t j = 0; j < HEX_DUMP_WIDTH && i + j < len; j++) {
            uint8_t c = buf[i + j];
            printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
        }
        printf("\n");
    }
}

/* ─────────────────────────── Receive (select / poll) ───────────────────── */

#ifdef USE_POLL
/**
 * receive_poll() — waits for incoming data using poll(), then reads it.
 *
 * poll() is preferred on systems with many open file descriptors because
 * it doesn't have the fd_set size limitation that select() has.
 *
 * @param fd        Open, configured UART file descriptor
 * @param timeout_s Seconds to wait before declaring a timeout
 * @return          Number of bytes received, 0 on timeout, -1 on error
 */
static ssize_t receive_poll(int fd, int timeout_s)
{
    struct pollfd pfd = {
        .fd     = fd,
        .events = POLLIN  /* notify when data is available to read */
    };

    printf("[uart_test] Waiting for RX data (poll, timeout=%ds)...\n", timeout_s);

    int ret = poll(&pfd, 1, timeout_s * 1000 /* milliseconds */);

    if (ret < 0) {
        if (errno == EINTR) {
            printf("[uart_test] poll() interrupted by signal\n");
            return 0;
        }
        fprintf(stderr, "[ERROR] poll() failed: %s\n", strerror(errno));
        return -1;
    }

    if (ret == 0) {
        printf("[uart_test] RX timeout — no data received within %ds\n", timeout_s);
        return 0;
    }

    if (pfd.revents & POLLERR) {
        fprintf(stderr, "[ERROR] poll() reported error on fd\n");
        return -1;
    }

    /* Data is ready — read it */
    uint8_t buf[RX_BUF_SIZE];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        fprintf(stderr, "[ERROR] read() failed: %s\n", strerror(errno));
        return -1;
    }

    buf[n] = '\0'; /* null-terminate for printf */

    printf("[uart_test] RX (%zd bytes):\n", n);
    hex_dump(buf, (size_t)n);
    printf("[uart_test] RX ASCII: %s\n", buf);

    return n;
}

#else   /* default: use select() */

/**
 * receive_select() — waits for incoming data using select(), then reads it.
 *
 * select() watches the fd for readability with a caller-specified timeout.
 * It is POSIX-standard and works on virtually all Linux systems.
 *
 * @param fd        Open, configured UART file descriptor
 * @param timeout_s Seconds to wait before declaring a timeout
 * @return          Number of bytes received, 0 on timeout, -1 on error
 */
static ssize_t receive_select(int fd, int timeout_s)
{
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);  /* watch our UART fd for incoming data */

    struct timeval tv = {
        .tv_sec  = timeout_s,
        .tv_usec = 0
    };

    printf("[uart_test] Waiting for RX data (select, timeout=%ds)...\n", timeout_s);

    /*
     * select() blocks until:
     *  (a) data arrives on fd   → returns > 0
     *  (b) timeout expires      → returns 0
     *  (c) signal interrupts    → returns -1, errno=EINTR
     *  (d) error                → returns -1, errno≠EINTR
     */
    int ret = select(fd + 1, &read_fds, NULL, NULL, &tv);

    if (ret < 0) {
        if (errno == EINTR) {
            printf("[uart_test] select() interrupted by signal\n");
            return 0;
        }
        fprintf(stderr, "[ERROR] select() failed: %s\n", strerror(errno));
        return -1;
    }

    if (ret == 0) {
        printf("[uart_test] RX timeout — no data received within %ds\n", timeout_s);
        return 0;
    }

    /* FD_ISSET check is good practice even when only one fd is watched */
    if (!FD_ISSET(fd, &read_fds)) {
        fprintf(stderr, "[ERROR] select() returned but fd not in set\n");
        return -1;
    }

    /* Data is ready — read whatever is in the kernel RX buffer */
    uint8_t buf[RX_BUF_SIZE];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        fprintf(stderr, "[ERROR] read() failed: %s\n", strerror(errno));
        return -1;
    }
    if (n == 0) {
        printf("[uart_test] read() returned 0 (remote end closed)\n");
        return 0;
    }

    buf[n] = '\0'; /* null-terminate for safe printf */

    printf("[uart_test] RX (%zd bytes):\n", n);
    hex_dump(buf, (size_t)n);
    printf("[uart_test] RX ASCII: %s\n", buf);

    return n;
}
#endif  /* USE_POLL */

/* ─────────────────────────── Loopback Self-Test ────────────────────────── */

/**
 * run_loopback_test() — transmits TEST_MESSAGE and verifies the received
 * bytes match exactly what was sent.
 *
 * This requires either:
 *  (a) A physical TX→RX wire jumper on the UART header, OR
 *  (b) A virtual socat pty pair where both ends are the same "device"
 *
 * On a RISC-V board, a loopback test is a quick sanity check that the
 * UART driver, baud rate, and wiring are all correct before running ACT.
 *
 * @param fd  Open, configured UART file descriptor
 * @return    0 if loopback passes, -1 on failure or mismatch
 */
static int run_loopback_test(int fd)
{
    printf("\n[uart_test] ── LOOPBACK TEST ──────────────────────────────\n");
    printf("[uart_test] TX→RX jumper (or socat pair) must be in place.\n\n");

    const char *msg   = TEST_MESSAGE;
    size_t      txlen = strlen(msg);

    /* Transmit the test message */
    if (transmit(fd, msg) < 0)
        return -1;

    /* Small delay to let the echoed bytes arrive in the RX buffer */
    usleep(100 * 1000); /* 100 ms */

    /* Read back whatever arrived */
    uint8_t rxbuf[RX_BUF_SIZE];
    memset(rxbuf, 0, sizeof(rxbuf));

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = { .tv_sec = RX_TIMEOUT_SEC, .tv_usec = 0 };

    int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (ret <= 0) {
        fprintf(stderr,
            "[FAIL] Loopback: no data received within %ds.\n"
            "  → Is TX physically connected to RX?\n"
            "  → For socat virtual test, run both ends on the SAME pty pair.\n",
            RX_TIMEOUT_SEC);
        return -1;
    }

    ssize_t n = read(fd, rxbuf, sizeof(rxbuf) - 1);
    if (n < 0) {
        fprintf(stderr, "[FAIL] Loopback read error: %s\n", strerror(errno));
        return -1;
    }

    /* Compare TX vs RX */
    if ((size_t)n == txlen && memcmp(rxbuf, msg, txlen) == 0) {
        printf("[PASS] Loopback test PASSED — TX matches RX (%zd bytes)\n\n", n);
        return 0;
    } else {
        fprintf(stderr,
            "[FAIL] Loopback mismatch!\n"
            "  TX (%zu bytes): %s"
            "  RX (%zd bytes): %.*s\n",
            txlen, msg, n, (int)n, rxbuf);
        return -1;
    }
}

/* ─────────────────────────── Usage / Main ──────────────────────────────── */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <device> <baud_rate> [--loopback]\n\n"
        "Examples:\n"
        "  %s /dev/ttyUSB0 115200\n"
        "  %s /dev/ttyUSB0 115200 --loopback\n"
        "  %s /dev/pts/3   9600\n\n"
        "Supported baud rates: 1200 2400 4800 9600 19200 38400\n"
        "                       57600 115200 230400 460800 921600\n\n"
        "Compile with -DUSE_POLL to use poll() instead of select().\n",
        prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
    /* ── Argument parsing ────────────────────────────────────────────────── */

    if (argc < 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *device    = argv[1];
    int         baud_rate = atoi(argv[2]);
    bool        loopback  = (argc >= 4 && strcmp(argv[3], "--loopback") == 0);

    if (baud_rate <= 0) {
        fprintf(stderr, "[ERROR] Invalid baud rate '%s'\n", argv[2]);
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    /* ── Register cleanup handlers ───────────────────────────────────────── */

    /*
     * atexit() guarantees restore_terminal() runs even if exit() is called
     * from deep inside helper functions — important for clean shutdown on
     * RISC-V hardware where leaving the port mis-configured affects other tools.
     */
    if (atexit(restore_terminal) != 0) {
        fprintf(stderr, "[ERROR] atexit() registration failed\n");
        return EXIT_FAILURE;
    }

    /* Catch Ctrl+C — exit() will trigger atexit chain */
    signal(SIGINT,  sigint_handler);
    signal(SIGTERM, sigint_handler);

    /* ── Open and configure port ─────────────────────────────────────────── */

    printf("\n[uart_test] ── UART TEST PROGRAM ──────────────────────────\n");
#ifdef USE_POLL
    printf("[uart_test] I/O mode : poll()\n");
#else
    printf("[uart_test] I/O mode : select()\n");
#endif
    printf("[uart_test] Device   : %s\n", device);
    printf("[uart_test] Baud rate: %d\n", baud_rate);
    printf("[uart_test] Loopback : %s\n\n", loopback ? "YES" : "NO");

    g_uart_fd = open_uart(device);
    if (g_uart_fd < 0)
        return EXIT_FAILURE;

    if (configure_uart(g_uart_fd, baud_rate) < 0)
        return EXIT_FAILURE;

    /* ── Run loopback test OR normal TX/RX ──────────────────────────────── */

    if (loopback) {
        return (run_loopback_test(g_uart_fd) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    /* Normal mode: transmit test message, then listen for a response */
    printf("\n[uart_test] ── TRANSMIT ────────────────────────────────────\n");
    if (transmit(g_uart_fd, TEST_MESSAGE) < 0)
        return EXIT_FAILURE;

    printf("\n[uart_test] ── RECEIVE ─────────────────────────────────────\n");

#ifdef USE_POLL
    ssize_t rx_bytes = receive_poll(g_uart_fd, RX_TIMEOUT_SEC);
#else
    ssize_t rx_bytes = receive_select(g_uart_fd, RX_TIMEOUT_SEC);
#endif

    printf("\n[uart_test] ── DONE ─────────────────────────────────────────\n");
    if (rx_bytes > 0)
        printf("[uart_test] Session complete. Received %zd bytes.\n", rx_bytes);
    else
        printf("[uart_test] Session complete. No data received.\n");

    /* atexit() → restore_terminal() will run after this return */
    return EXIT_SUCCESS;
}
