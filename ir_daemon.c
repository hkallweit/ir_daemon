#define _GNU_SOURCE

#include <unistd.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <syslog.h>
#include <signal.h>
#include <string.h>
#include <ctype.h>
#include <sys/wait.h>
#include <libevdev-1.0/libevdev/libevdev.h>

#define log(level, fmt, ...) \
	syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_##level), fmt, ##__VA_ARGS__)

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

static const char command[] = "/tmp/ir.sh";
static const char pid_file[] = "/var/run/ir_daemon.pid";
static char *dev_name;
static struct libevdev *dev;
static int fd_pid = -1;

static volatile sig_atomic_t fatal_signal_in_progress;

void sig_handler(int sig)
{
	if (fatal_signal_in_progress)
		raise(sig);

	fatal_signal_in_progress = 1;

	log(WARNING, "caught signal %s, exiting ..\n", strsignal(sig));
	if (dev)
		libevdev_free(dev);

	signal(sig, SIG_DFL);
	raise(sig);
	if (fd_pid >= 0)
		unlink(pid_file);
}

static void run_command(const char *key_name)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid == 0) {
		execl(command, command, key_name, NULL);
		log(WARNING, "error executing command %s: %s\n",
		    command, strerror(errno));
		_exit(EXIT_FAILURE);
	} else if (pid < 0) {
		log(ERR, "fork failed\n");
	} else {
		if (waitpid(pid, &status, 0) != pid)
			log(ERR, "error waiting for command to be finished\n");
		else if (!WIFEXITED(status) || WEXITSTATUS(status))
			log(WARNING, "command returned with retcode != 0\n");
	}
}

static void action(__u16 key_code)
{
	static const struct {
		const char *key_name;
		__u16 key_code;
		}	keys[] = {
		{ "KEY_PREVIOUS", KEY_PREVIOUS },
		{ "KEY_NEXT", KEY_NEXT },
		{ "KEY_BACK", KEY_BACK },
		{ "KEY_FORWARD", KEY_FORWARD },
		{ "KEY_PLAY", KEY_PLAY },
		{ "KEY_PAUSE", KEY_PAUSE },
		{ "KEY_STOP", KEY_STOP },
		{ "KEY_ENTER", KEY_ENTER },
	};

	int i;

	for (i = 0; i < ARRAY_SIZE(keys); i++)
		if (key_code == keys[i].key_code) {
			run_command(keys[i].key_name);
			return;
		}
	log(DEBUG, "no command associated to keycode %u\n", key_code);
}

int get_opts(int argc, char * const *argv)
{
	int c, ret;
	const char *dev = NULL;

	opterr = 0;

	while((c = getopt(argc, argv, "d:")) != -1)
		switch (c) {
		case 'd':
			dev = optarg;
			break;
		case '?':
			if (optopt == 'd')
				log(WARNING, "option -%c requires an argument\n", optopt);
			else if (isprint(optopt))
				log(WARNING, "unknown option -%c\n", optopt);
			else
				log(WARNING, "unknown option character 0x%x\n", optopt);
			break;
		default:
			log(ERR, "unknown error getting opts\n");
		}

	ret = asprintf(&dev_name, "/dev/input/%s", dev ?: "ir");
	if (ret < 0) {
		log(ERR, "error getting input device name\n");
		return -1;
	}

	return 0;
}

int main(int argc, char *argv[])
{
	int ret;
	int fd;
	int num_chars;
	ssize_t fret;
	struct pollfd pollfd;
	struct libevdev *dev = NULL;
	char pid_buf[32];

	signal(SIGHUP, sig_handler);
	signal(SIGQUIT, sig_handler);
	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGABRT, sig_handler);
	signal(SIGBUS, sig_handler);
	signal(SIGSEGV, sig_handler);

	ret = get_opts(argc, argv);
	if (ret)
		return 1;

	fd = open(dev_name, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		log(ERR, "can't open input: %s\n", strerror(errno));
		return 1;
	}

	ret = libevdev_new_from_fd(fd, &dev);
	if (ret < 0) {
		log(ERR, "can't open input: %s\n", strerror(-ret));
		goto out_fd_close;
	}

	ret = daemon(0, 0);
	if (ret) {
		log(ERR, "error becoming a daemon\n");
		goto out_evdev_free;
	}

	fd_pid = open(pid_file, O_WRONLY | O_CREAT | O_EXCL,
		      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (fd_pid < 0) {
		if (errno == EEXIST)
			log(ERR, "can't start, PID file exists already\n");
		else
			log(ERR, "can't create PID file: %s\n", strerror(errno));
		goto out_evdev_free;
	}

	num_chars = sprintf(pid_buf, "%d\n", getpid());
	fret = write(fd_pid, pid_buf, num_chars);
	if (fret != num_chars) {
		log(ERR, "error writing PID to file\n");
		goto out_evdev_free;
	}
	close(fd_pid);

	log(INFO, "ir_daemon started successfully\n");

	pollfd.fd = fd;
	pollfd.events = POLLIN;

start:
	pollfd.revents = 0;
	ret = poll(&pollfd, 1, -1);
	/* poll timed out, should not happen */
	if (!ret)
		goto start;
	if (ret < 0) {
		log(ERR, "poll failed: %s\n", strerror(errno));
		goto out_evdev_free;
	}

	/* FIXME: check revents in more detail */
	if (!(pollfd.revents & POLLIN)) {
		log(WARNING, "poll returned successfully but with no data\n");
		goto out_evdev_free;
	}

	do {
		struct input_event ev;
		ret = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
		switch (ret) {
		case LIBEVDEV_READ_STATUS_SUCCESS:
			if (ev.type == EV_KEY && ev.value)
				action(ev.code);
			break;
		case LIBEVDEV_READ_STATUS_SYNC:
			while (libevdev_next_event(dev, LIBEVDEV_READ_FLAG_SYNC, &ev)
				== LIBEVDEV_READ_STATUS_SYNC);
			break;
		case -EAGAIN:
			break;
		default:
			log(ERR, "libevdev_next failed: %s\n", strerror(-ret));
			goto out_evdev_free;
		}
	} while (ret != -EAGAIN);
	goto start;


out_evdev_free:
	libevdev_free(dev);
out_fd_close:
	close(fd);
	if (fd_pid >= 0)
		unlink(pid_file);

	return 0;
}
