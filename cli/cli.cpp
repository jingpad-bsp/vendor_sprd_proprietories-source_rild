/**
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#define LOG_TAG "RIL_CLI"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <log/log.h>
#include "atci.h"

#define MAX_COMMAND_BYTES (8 * 1024)

enum simList {
    CLI_SIM1,
    CLI_SIM2,
    CLI_SIM_MAX
};

enum simOperation {
    CLI_PLUG_IN_SIM,
    CLI_PLUG_OUT_SIM,
    CLI_FILE_REFRESH_0,
    CLI_FILE_REFRESH_1,
    CLI_FILE_REFRESH_2,
    CLI_SIM_MAX_OP
};

char simCmd[5][32] = {
    "SIM Hot Plug In",
    "SIM Hot Plug Out",
    "SIM File Refresh 0",
    "SIM File Refresh 1",
    "SIM File Refresh 2"
};

typedef void (*modTest)(int argc, char **param, int sim);

struct module{
    char name[16];
    modTest mod;
};

void simCli(int argc, char **param, int simId);

module modArray[] = {
    {"sim", (modTest) simCli},
};

char *parseArgv(const char *option, char **argv, int argc) {
    char *data = NULL;
    if ((argv == NULL) || (option == NULL)) {
        RLOGE("The argv is NULL");
        return data;
    }

    for (int i = 1; i < argc; i++) {
        // RLOGD("The argv[%d] is %s", i, argv[i]);
        if ((0 == strncasecmp(argv[i], option, strlen(option))) && ((argc - i) > 1)) {
            data = argv[i+1];
            RLOGE("The data for argv is %s\n", data);
            break;
        }
    }
    return data;
}

int getModule(char *module) {
    int mod = -1;
    int len = sizeof(modArray) / sizeof(modArray[0]);
    RLOGD("The len for modArray is %d", len);
    for (int i = 0; i < len; i++) {
        if (0 == strncasecmp(modArray[i].name, module, strlen(module))) {
            mod = i;
            break;
        }
    }
    return mod;
}

void simCli(int argc, char **param, int simId) {
    int IOperation = -1;
    char *SOperation = NULL;
    char result[MAX_COMMAND_BYTES] = {0};
    const char *opt = "-o";
    SOperation = parseArgv(opt, param, argc);
    if (SOperation == NULL) {
        RLOGE("The parameter for sim test is error");
        return;
    } else {
        IOperation = atoi(SOperation);
        RLOGD("The operation for sim test is %d", IOperation);
    }
    if ((IOperation < CLI_PLUG_IN_SIM) || (IOperation >= CLI_SIM_MAX_OP)) {
        RLOGE("Unsupported the operation!");
        return;
    }

    sendATCmd(simId, simCmd[IOperation], result, MAX_COMMAND_BYTES);
}

int main(int argc, char **argv) {
    int sim = -1, module = -1;
    char *simArg = NULL;
    const char *simOpt = "-s";

    RLOGD("**Test hot plug sim Started**");
    RLOGD("**param count = %d**", argc);

    if ((argv == NULL) || argc < 1) {
        RLOGE("The parameter is NULL");
        return -1;
    }

    RLOGD("**argv[0] = %s**", argv[0]);

    simArg = parseArgv(simOpt, argv, argc);
    if (simArg != NULL) {
        sim = atoi(simArg);
        RLOGD("sim = %d in %s", sim, __FUNCTION__);
    } else {
        RLOGE("The parameter sim is NULL");
        return -1;
    }

    if ((sim < CLI_SIM1) || (sim >= CLI_SIM_MAX)) {
        RLOGE("Unsupported sim!");
        return -1;
    }

    // module parameter use moudle name
    module = getModule(argv[1]);

    if ((module < 0)) {
        RLOGE("Unsupported module!");
        return -1;
    }
    modArray[module].mod(argc, argv, sim);

    return 0;
}
