/*
 * umigratord launches the target application as its child, runs the MigFlow
 * policy on the child's pages with move_pages(2) and exits with the child.
 */
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <argp.h>
#include "migflow.h"

static struct opts my_opts;

static struct argp_option options[] = {
	{ "quick-demotion", 'q', "0|1", 0, "enable quick demotion of cooled pages (default 1)", 0 },
	{ "dynamic-alpha", 'a', "0|1", 0, "scale the migration cost with the tier-0 hit ratio (default 1)", 0 },
	{ "alpha-min", 'l', "FLOAT", 0, "lower bound of the dynamic alpha (default 1.0)", 0 },
	{ "print-interval", 'i', "SECONDS", 0, "print statistics periodically (default: only at exit)", 0 },
	{ "verbose", 'v', "LEVEL", 0, "0 = summary and one line per migration phase (default), 1 = every migration step, 2 = debug", 0 },
	{ 0, 0, 0, 0, 0, 0 },
};

static error_t parse_option(int key, char *arg, struct argp_state *state)
{
	struct opts *opts = (struct opts *)state->input;

	switch (key) {
	case 'q':
		opts->do_quick_demotion = atoi(arg);
		break;
	case 'a':
		opts->dyn_alpha = atoi(arg);
		break;
	case 'l':
		opts->alpha_min = atof(arg);
		break;
	case 'i':
		opts->print_itv = atoi(arg);
		break;
	case 'v':
		opts->verbose_level = atoi(arg);
		break;
	case ARGP_KEY_ARGS:
		/* the program and its arguments */
		opts->exename = state->argv[state->next];
		opts->idx = state->next;
		break;
	case ARGP_KEY_NO_ARGS:
		argp_usage(state);
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	struct argp argp = {
		.options = options,
		.parser = parse_option,
		.args_doc = "[--] PROGRAM [ARGS...]",
		.doc = "umigratord -- MigFlow userspace migration daemon",
		.children = NULL,
		.help_filter = NULL,
		.argp_domain = NULL,
	};

	my_opts.do_quick_demotion = 1;
	my_opts.dyn_alpha = 1;
	my_opts.alpha_min = 1.0;
	my_opts.print_itv = -1;
	my_opts.verbose_level = LOG_ALWAYS;

	argp_parse(&argp, argc, argv, ARGP_IN_ORDER, NULL, &my_opts);
	argv += my_opts.idx;

	/* The child starts after the daemon is attached to it. */
	int sync_pipe[2];
	if (pipe(sync_pipe) < 0) {
		perror("pipe");
		return -1;
	}

	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		char go = 0;

		close(sync_pipe[1]);
		if (read(sync_pipe[0], &go, 1) != 1 || go != 'g')
			_exit(1);
		close(sync_pipe[0]);
		execv(my_opts.exename, argv);
		perror(my_opts.exename);
		_exit(1);
	}
	close(sync_pipe[0]);

	signal(SIGINT, SIG_IGN);
	if (migflow_init(pid, &my_opts) < 0) {
		fprintf(stderr, "umigratord: initialization failed (is migflow.ko loaded?)\n");
		close(sync_pipe[1]);	/* the child exits without running */
		wait(NULL);
		return -1;
	}
	if (write(sync_pipe[1], "g", 1) != 1)
		perror("write");
	close(sync_pipe[1]);
	printf("umigratord pid %d, application pid %d\n", getpid(), pid);
	wait(NULL);
	migflow_destroy();
	return 0;
}
