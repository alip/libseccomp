/**
 * BPF Simulator
 *
 * Copyright (c) 2012 Red Hat <pmoore@redhat.com>
 * Author: Paul Moore <pmoore@redhat.com>
 */

/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "bpf.h"

#define BPF_PRG_MAX_LEN		4096

/**
 * BPF simulator machine state
 */
struct sim_state {
	uint32_t acc;
	uint32_t temp[BPF_SCRATCH_SIZE];
};

struct bpf_program {
	size_t i_cnt;
	struct bpf_instr *i;
};

static unsigned int opt_verbose = 0;
static unsigned int opt_machine = 32;

/**
 * Print the usage information to stderr and exit
 * @param program the name of the current program being invoked
 *
 * Print the usage information and exit with EINVAL.
 *
 */
static void exit_usage(const char *program)
{
	fprintf(stderr, "usage: %s [-m {32,64}] -f <bpf_file> [-v]"
			" -s <syscall_num> [-0 <a0>] ... [-5 <a5>]",
			program);
	exit(EINVAL);
}

/**
 * Handle a simulator fault
 * @param rc the error or return code
 *
 * Print a "FAULT" to stderr to indicate a simulator fault, and an errno value
 * if the simulator is running in verbose mode, then exit with EFAULT.
 *
 */
static void exit_fault(unsigned int rc)
{
	if (opt_verbose)
		fprintf(stderr, "FAULT: errno = %d\n", rc);
	else
		fprintf(stderr, "FAULT\n");
	exit(EFAULT);
}

/**
 * Handle a BPF program error
 * @param rc the error or return code
 * @param line the line number
 *
 * Print an "ERROR" to stderr to indicate a program error, and an errno value
 * if the simulator is running in verbose mode, then exit with ENOEXEC.
 *
 */
static void exit_error(unsigned int rc, unsigned int line)
{
	if (opt_verbose)
		fprintf(stderr, "ERROR: errno = %d, line = %d\n", rc, line);
	else
		fprintf(stderr, "ERROR\n");
	exit(ENOEXEC);
}

/**
 * Handle a simulator return/action
 * @param action the return value
 * @param line the line number
 *
 * Display the action to stdout and exit with 0.
 *
 */
static void end_action(uint32_t action, unsigned int line)
{
	if (action == 0x00000000)
		fprintf(stdout, "KILL");
	else if (action == 0x00020000)
		fprintf(stdout, "TRAP");
	else if ((action & 0xffff0000) == 0x00030000)
		fprintf(stdout, "ERRNO(%u)", (action & 0x0000ffff));
	else if (action == 0x7fff0000)
		fprintf(stdout, "ALLOW");
	else
		exit_error(EDOM, line);
	exit(0);
}

/**
 * Execute a BPF program
 * @param prg the loaded BPF program
 * @param sys_data the syscall record being tested
 *
 * Simulate the BPF program with the given syscall record.
 *
 */
static void bpf_execute(const struct bpf_program *prg,
			const struct bpf_syscall_data *sys_data)
{
	unsigned int ip, ip_c;
	struct sim_state state;
	struct bpf_instr *bpf;
	unsigned char *sys_data_b = (unsigned char *)sys_data;

	/* initialize the machine state */
	ip_c = 0;
	ip = 0;
	memset(&state, 0, sizeof(state));

	/* start execution */
	while (ip < prg->i_cnt) {
		/* get the instruction and bump the ip */
		ip_c = ip;
		bpf = &prg->i[ip++];

		switch (bpf->op) {
		case BPF_LD+BPF_W+BPF_ABS:
			if ((opt_machine == 32) &&
			    (bpf->k < BPF_SYSCALL_MAX_32)) {
				state.acc = sys_data_b[bpf->k];
			} else if ((opt_machine == 64) &&
			    (bpf->k < BPF_SYSCALL_MAX_64)) {
				state.acc = sys_data_b[bpf->k];
			} else
				exit_error(ERANGE, ip_c);
			break;
		case BPF_JMP+BPF_JA:
			ip += bpf->k;
			break;
		case BPF_JMP+BPF_JEQ+BPF_K:
			if (bpf->k == state.acc)
				ip += bpf->jt;
			else
				ip += bpf->jf;
			break;
		case BPF_JMP+BPF_JGT+BPF_K:
			if (bpf->k > state.acc)
				ip += bpf->jt;
			else
				ip += bpf->jf;
			break;
		case BPF_JMP+BPF_JGE+BPF_K:
			if (bpf->k >= state.acc)
				ip += bpf->jt;
			else
				ip += bpf->jf;
			break;
		case BPF_RET+BPF_K:
			end_action(bpf->k, ip_c);
			break;
		default:
			/* XXX - since we don't support the full bpf language
			 *       just yet, this could be either a fault or
			 *       and error, we'll treat it as a fault until we
			 *       provide full bpf support */
			exit_fault(EOPNOTSUPP);
		}
	}

	/* if we've reached here there is a problem with the program */
	exit_error(ERANGE, ip_c);
}

/**
 * main
 */
int main(int argc, char *argv[])
{
	int opt;
	int fd, fd_read_len;
	char *opt_file = NULL;
	unsigned int opt_arg_flag = 0;
	struct bpf_syscall_data sys_data;
	struct bpf_program bpf_prg;

	/* clear the syscall record */
	memset(&sys_data, 0, sizeof(sys_data));

	/* parse the command line */
	while ((opt = getopt(argc, argv, "f:h:m:s:v0:1:2:3:4:5:")) > 0) {
		switch (opt) {
		case 'f':
			opt_file = strdup(optarg);
			if (opt_file == NULL)
				exit_fault(ENOMEM);
			break;
		case 'm':
			if (opt_arg_flag)
				exit_usage(argv[0]);
			opt_machine = strtol(optarg, NULL, 0);
			if (opt_machine != 32 && opt_machine != 64)
				exit_usage(argv[0]);
			break;
		case 's':
			sys_data.sys = strtol(optarg, NULL, 0);
			break;
		case 'v':
			opt_verbose = 1;
			break;
		case '0':
			opt_arg_flag = 1;
			if (opt_machine == 32)
				sys_data.args.m32[0] = strtol(optarg, NULL, 0);
			else if (opt_machine == 64)
				sys_data.args.m64[0] = strtol(optarg, NULL, 0);
			else
				exit_fault(EINVAL);
			break;
		case '1':
			opt_arg_flag = 1;
			if (opt_machine == 32)
				sys_data.args.m32[1] = strtol(optarg, NULL, 0);
			else if (opt_machine == 64)
				sys_data.args.m64[1] = strtol(optarg, NULL, 0);
			else
				exit_fault(EINVAL);
			break;
		case '2':
			opt_arg_flag = 1;
			if (opt_machine == 32)
				sys_data.args.m32[2] = strtol(optarg, NULL, 0);
			else if (opt_machine == 64)
				sys_data.args.m64[2] = strtol(optarg, NULL, 0);
			else
				exit_fault(EINVAL);
			break;
		case '3':
			opt_arg_flag = 1;
			if (opt_machine == 32)
				sys_data.args.m32[3] = strtol(optarg, NULL, 0);
			else if (opt_machine == 64)
				sys_data.args.m64[3] = strtol(optarg, NULL, 0);
			else
				exit_fault(EINVAL);
			break;
		case '4':
			opt_arg_flag = 1;
			if (opt_machine == 32)
				sys_data.args.m32[4] = strtol(optarg, NULL, 0);
			else if (opt_machine == 64)
				sys_data.args.m64[4] = strtol(optarg, NULL, 0);
			else
				exit_fault(EINVAL);
			break;
		case '5':
			opt_arg_flag = 1;
			if (opt_machine == 32)
				sys_data.args.m32[5] = strtol(optarg, NULL, 0);
			else if (opt_machine == 64)
				sys_data.args.m64[5] = strtol(optarg, NULL, 0);
			else
				exit_fault(EINVAL);
			break;
		case 'h':
		default:
			/* usage information */
			exit_usage(argv[0]);
		}
	}

	/* allocate space for the bpf program */
	/* XXX - we should make this dynamic */
	bpf_prg.i_cnt = 0;
	bpf_prg.i = calloc(BPF_PRG_MAX_LEN, sizeof(*bpf_prg.i));
	if (bpf_prg.i == NULL)
		exit_fault(ENOMEM);

	/* load the bpf program */
	fd = open(opt_file, 0);
	if (fd < 0)
		exit_fault(errno);
	do {
		/* XXX - need to account for partial reads */
		fd_read_len = read(fd, &(bpf_prg.i[bpf_prg.i_cnt]),
				   sizeof(*bpf_prg.i));
		if (fd_read_len == sizeof(*bpf_prg.i))
			bpf_prg.i_cnt++;

		/* check the size */
		if (bpf_prg.i_cnt == BPF_PRG_MAX_LEN)
			exit_fault(E2BIG);
	} while (fd_read_len > 0);
	close(fd);

	/* execute the bpf program */
	bpf_execute(&bpf_prg, &sys_data);

	/* we should never reach here */
	exit_fault(EFAULT);
	return 0;
}
