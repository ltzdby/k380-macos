/*
 * k380-listener: consume com.apple.iokit.matching events and re-apply the K380
 * F-key mode whenever the keyboard connects.
 *
 * Why this exists: the K380 does not persist its F-key mode (it resets on
 * power-off, sleep, and every Bluetooth reconnect). A launchd LaunchEvents job
 * that runs a plain script relaunches every ~10s forever, because IOKit match
 * events MUST be consumed via xpc_set_event_stream_handler() (see xpc_events(3))
 * and a script cannot do that. This tiny program registers that handler so the
 * events are consumed, then runs the existing (already Input-Monitoring
 * authorized) `k380` binary to perform the seize/write.
 *
 * Applying is idempotent (it writes a fixed "F-keys on" state, not a toggle) and
 * takes well under a second, so we simply re-apply on every connect event. The
 * child's combined output is captured into the single log below, so there is
 * nothing to redirect from the plist.
 *
 * This is launched on demand by launchd (no RunAtLoad/KeepAlive): it processes
 * the connect event(s) and then exits once the event burst has settled, so no
 * process lingers between connects. A single connect exposes several HID
 * collections that each fire an event; the idle timer coalesces them into one
 * short-lived run.
 */

#include <xpc/xpc.h>
#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* Absolute path to the k380 binary, resolved at startup as a sibling of this
 * executable (see resolve_k380_bin). The k380 binary, this listener, and
 * libhidapi.dylib are all expected to live in the same directory. */
static char k380_bin[PATH_MAX];

/* Locate the k380 binary next to our own executable so nothing is hardcoded and
 * the whole directory can be relocated freely. */
static void resolve_k380_bin(void)
{
	char exe[PATH_MAX];
	uint32_t size = sizeof(exe);
	if (_NSGetExecutablePath(exe, &size) != 0) {
		/* Buffer too small: PATH_MAX should never overflow for a real install. */
		snprintf(k380_bin, sizeof(k380_bin), "k380");
		return;
	}
	snprintf(k380_bin, sizeof(k380_bin), "%s/k380", dirname(exe));
}

/* Append a timestamped line to ~/Library/Logs/k380-fnkeys.log. To keep the file
 * bounded without any external rotation, it is truncated the first time it is
 * written in a new calendar month (detected via the previous mtime). */
static void logmsg(const char *fmt, ...)
{
	char path[1024];
	const char *home = getenv("HOME");
	if (home)
		snprintf(path, sizeof(path), "%s/Library/Logs/k380-fnkeys.log", home);
	else
		snprintf(path, sizeof(path), "/tmp/k380-fnkeys.log");

	time_t now = time(NULL);

	const char *mode = "a";
	struct stat st;
	if (stat(path, &st) == 0) {
		struct tm last, cur;
		localtime_r(&st.st_mtime, &last);
		localtime_r(&now, &cur);
		if (last.tm_year != cur.tm_year || last.tm_mon != cur.tm_mon)
			mode = "w";
	}

	FILE *f = fopen(path, mode);
	if (!f)
		return;

	char when[32];
	strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
	fprintf(f, "[%s] ", when);

	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);

	fputc('\n', f);
	fclose(f);
}

/* Run `sudo -n <k380> -f on`, capture its combined stdout+stderr into out (with
 * newlines flattened to keep the log one line), and return its exit status
 * (-1 on spawn failure). No shell is used so the process chain stays
 * launchd -> listener -> sudo -> k380, which is what Input Monitoring / TCC and
 * the NOPASSWD sudoers entry are authorized against. */
static int apply_fkeys(char *out, size_t outsz)
{
	int pipefd[2];
	out[0] = '\0';
	if (pipe(pipefd) != 0)
		return -1;

	pid_t pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		execl("/usr/bin/sudo", "sudo", "-n", k380_bin, "-f", "on", (char *)NULL);
		_exit(127);
	}

	close(pipefd[1]);
	ssize_t r = read(pipefd[0], out, outsz - 1);
	size_t n = (r > 0) ? (size_t)r : 0;
	out[n] = '\0';
	close(pipefd[0]);

	for (size_t i = 0; i < n; i++)
		if (out[i] == '\n' || out[i] == '\r')
			out[i] = ' ';
	while (n > 0 && out[n - 1] == ' ')
		out[--n] = '\0';

	int status;
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Seconds of quiet (no new connect event) after which the process exits. One
 * physical connect fans out into several IOKit events a fraction of a second
 * apart; this window lets them coalesce into a single run before we exit. */
#define IDLE_EXIT_SECS 3

static void handle_event(xpc_object_t event)
{
	if (xpc_get_type(event) != XPC_TYPE_DICTIONARY) {
		logmsg("ignored non-dictionary event");
		return;
	}

	const char *name = xpc_dictionary_get_string(event, XPC_EVENT_KEY_NAME);
	if (!name)
		name = "?";

	char out[256];
	int rc = apply_fkeys(out, sizeof(out));
	logmsg("event %s -> k380 -f on: rc=%d (%s)", name, rc, out);
}

int main(void)
{
	resolve_k380_bin();
	logmsg("listener started (pid %d, k380=%s)", getpid(), k380_bin);

	/* Serial queue so overlapping connect events (the K380 exposes several HID
	 * collections, each firing its own event) are handled one at a time. */
	dispatch_queue_t q = dispatch_queue_create("com.local.k380", DISPATCH_QUEUE_SERIAL);

	/* Idle timer: launchd starts us on demand for a connect, so exit once the
	 * burst of events has settled rather than lingering as a resident process.
	 * It is armed now (in case no event ever arrives) and re-armed on each event;
	 * when it fires we are done. A timer resumed without an initial value would
	 * fire immediately, so arm before resuming. The exact fire instant does not
	 * matter, so a generous leeway is fine. */
	dispatch_source_t idle = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
	dispatch_source_set_event_handler(idle, ^{
		exit(0);
	});
	void (^arm_idle)(void) = ^{
		dispatch_source_set_timer(idle,
			dispatch_time(DISPATCH_TIME_NOW, (int64_t)IDLE_EXIT_SECS * NSEC_PER_SEC),
			DISPATCH_TIME_FOREVER, NSEC_PER_SEC);
	};
	arm_idle();
	dispatch_resume(idle);

	xpc_set_event_stream_handler("com.apple.iokit.matching", q,
		^(xpc_object_t event) {
			handle_event(event);
			arm_idle();
		});

	dispatch_main();
	return 0;
}
