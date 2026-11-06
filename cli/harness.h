/* harness.h: agent-harness commands of the xksm CLI (see harness.c). */
#ifndef XKSM_CLI_HARNESS_H
#define XKSM_CLI_HARNESS_H

#include "xksm.h"

int xksm_cli_hook(const char *harness);
int xksm_cli_turn(int argc, char **argv);
int xksm_cli_context(int argc, char **argv);
int xksm_cli_transcripts(int argc, char **argv);
int xksm_cli_sessions(const char *harness);
int xksm_cli_forget(const char *harness, const char *session);
int xksm_cli_pause(const char *harness, const char *arg);
int xksm_cli_mcp(const char *harness);
int xksm_cli_mirror(void);
int xksm_cli_verify(void);
const char *xksm_cli_harness_version(void);

#endif
