#include <Achilles.h>
#include <exploit/exploit.h>

arg_t args[] = {
    // Name, short option, long option, description, examples, type, value
    {"Verbosity", "-v", "--verbosity", "Verbosity level, maximum of 2", "-vv, --verbosity 2", false, FLAG_INT, 0},
    {"Debug", "-d", "--debug", "Enable debug logging", NULL, false, FLAG_BOOL, false},
    {"Help", "-h", "--help", "Show this help message", NULL, false, FLAG_BOOL, false},
    {"Quick mode", "-q", "--quick", "Don't ask for confirmation during the program", NULL, false, FLAG_BOOL, false},
    {"Exploit", "-e", "--exploit", "Exploit with checkm8 and exit", NULL, false, FLAG_BOOL, false},
    {"PongoOS", "-p", "--pongo", "Boot to PongoOS and exit" , NULL, false, FLAG_BOOL, false},
    {"Jailbreak", "-j", "--jailbreak", "Jailbreak rootless using palera1n kpf, ramdisk and overlay", NULL, false, FLAG_BOOL, false},
    {"Verbose boot", "-V", "--verbose-boot", "Boot device with verbose boot", NULL, false, FLAG_BOOL, false},
    {"Serial output", "-s", "--serial", "Enable serial output from the device when booting", NULL, false, FLAG_BOOL, false},
    {"Boot arguments", "-b", "--boot-args", "Boot arguments to pass to PongoOS", NULL, false, FLAG_STRING, NULL},
    {"Override Pongo", "-k", "--override-pongo", "Use a custom Pongo.bin file", NULL, false, FLAG_STRING, NULL},
    {"Custom kernel patchfinder", "-K", "--custom-kpf", "Use a custom kernel patchfinder file", NULL, false, FLAG_STRING, NULL},
    {"Custom ramdisk", "-R", "--custom-ramdisk", "Use a custom ramdisk file", NULL, false, FLAG_STRING, NULL},
    #ifdef DEBUG
    {"Custom overlay", "-O", "--custom-overlay", "Use a custom overlay file", NULL, false, FLAG_STRING, NULL},
    {"Custom overwrite", "-o", "--custom-overwrite", "Use a custom overwrite file", NULL, false, FLAG_STRING, NULL},
    {"Custom payload", "-P", "--custom-payload", "Use a custom payload file", NULL, false, FLAG_STRING, NULL}
    #else
    {"Custom overlay", "-O", "--custom-overlay", "Use a custom overlay file", NULL, false, FLAG_STRING, NULL}
    #endif
};

arg_t *getArgumentByName(char *name)
{
    // Get an argument by its name
    for (int i = 0; i < sizeof(args) / sizeof(arg_t); i++)
    {
        if (strcmp(args[i].name, name) == 0)
        {
            return &args[i];
        }
    }
    return (arg_t *){NULL};
}

arg_t *findMatchingArgument(char *argv) {
    // Find a matching argument
    for (int i = 0; i < sizeof(args) / sizeof(arg_t); i++) {
        if (strcmp(argv, args[i].shortOpt) == 0 || strcmp(argv, args[i].longOpt) == 0) {
            return &args[i];
        }
    }
    return NULL;
}

void printHelp()
{
    // Print help information
    printf("Usage: %s [options]\n", NAME);
    printf("Options:\n");
    for (int i = 0; i < sizeof(args) / sizeof(arg_t); i++)
    {
        if (args[i].examples != NULL)
        {
            printf("\t%s, %s: %s (e.g. %s)\n", args[i].shortOpt, args[i].longOpt, args[i].description, args[i].examples);
        }
        else
        {
            printf("\t%s, %s: %s\n", args[i].shortOpt, args[i].longOpt, args[i].description);
        }
    }
}

bool checkForContradictions() {
    if (getArgumentByName("PongoOS")->boolVal && getArgumentByName("Jailbreak")->boolVal) {
        LOG(LOG_ERROR, "Cannot use both -p and -j");
        return true;
    }
    if (getArgumentByName("Exploit")->boolVal && (getArgumentByName("PongoOS")->boolVal || getArgumentByName("Jailbreak")->boolVal)) {
        LOG(LOG_ERROR, "Cannot use -e with -p or -j");
        return true;
    }
    if (getArgumentByName("Boot arguments")->stringVal != NULL && getArgumentByName("Boot arguments")->set && !getArgumentByName("Jailbreak")->boolVal) {
        LOG(LOG_ERROR, "Cannot use -b without -j");
        return true;
    }
    if (!getArgumentByName("Jailbreak")->boolVal && (getArgumentByName("Verbose boot")->boolVal || getArgumentByName("Serial output")->boolVal)) {
        LOG(LOG_ERROR, "Cannot use -V or -s without -j");
        return true;
    }
    if (getArgumentByName("Verbose boot")->boolVal && getArgumentByName("Serial output")->boolVal) {
        LOG(LOG_ERROR, "Cannot use -V and -s together");
        return true;
    }
    if (!getArgumentByName("Jailbreak")->boolVal && (getArgumentByName("Custom kernel patchfinder")->set || getArgumentByName("Custom ramdisk")->set || getArgumentByName("Custom overlay")->set)) {
        LOG(LOG_ERROR, "Cannot use -K, -R or -O without -j");
        return true;
    }
    return false;
}

bool parseMultipleShortArgs(char *multipleArgs) {
    bool unrecognisedOption = false;
    for (int i = 1; i < strlen(multipleArgs); i++) {
        char *newArg = malloc(sizeof(char) * 3);
        newArg[0] = '-';
        newArg[1] = multipleArgs[i];
        newArg[2] = '\0';
        arg_t *arg = findMatchingArgument(newArg);
        if (arg != NULL) {
            if (arg->type == FLAG_BOOL) {
                arg->boolVal = true;
                arg->set = true;
            } else if (arg->type == FLAG_INT) {
                arg->intVal++;
                arg->set = true;
            } else if (arg->type == FLAG_STRING) {
                LOG(LOG_ERROR, "Cannot use string argument '%s' with multiple short arguments", arg->name);
                unrecognisedOption = true;
            }
        } else {
            unrecognisedOption = true;
        }
    }
    return unrecognisedOption;
}

bool checkForUnrecognisedArguments(int argc, char *argv[]) {
    bool foundUnrecognisedArg = false;
    for (int i = 0; i < argc; i++) {
        if (i == 0) { continue; }
        if (i > 0) {
            if (findMatchingArgument(argv[i - 1]) != NULL
            && findMatchingArgument(argv[i - 1])->type == FLAG_STRING) {
                continue;
            }
            if (strstr(argv[i], "--") == NULL
            && strstr(argv[i], "-") != NULL
            && argv[i][0] == '-'
            && strlen(argv[i]) > 2) {
                foundUnrecognisedArg = parseMultipleShortArgs(argv[i]);
            } else if (findMatchingArgument(argv[i]) == NULL) {
                LOG(LOG_ERROR, "Unrecognised argument %s", argv[i]);
                foundUnrecognisedArg = true;
            }
        }
    }
    return foundUnrecognisedArg;
}

void parseArguments(int argc, char *argv[]) {
    for (int i = 0; i < argc; i++) {
        if (i == 0) { continue; }
        if (i > 0) {
            if (findMatchingArgument(argv[i - 1]) != NULL
            && findMatchingArgument(argv[i - 1])->type == FLAG_STRING) {
                arg_t *previousArg = findMatchingArgument(argv[i - 1]);
                if (previousArg->type == FLAG_STRING) {
                    previousArg->stringVal = argv[i];
                    previousArg->set = true;
                }
            }
            else {
                arg_t *arg = findMatchingArgument(argv[i]);
                if (arg == NULL) {
                    continue;
                }
                if (strstr(argv[i], "--") == NULL
                && strstr(argv[i], "-") != NULL
                && argv[i][0] == '-'
                && strlen(argv[i]) > 2) {
                    parseMultipleShortArgs(argv[i]);
                }
                    
                if (arg->type == FLAG_BOOL) {
                    arg->boolVal = true;
                    arg->set = true;
                }
                else if (arg->type == FLAG_INT) {
                    arg->intVal++;
                    arg->set = true;
                }
            }

        }
    }
}

int main(int argc, char *argv[])
{
    bool hasUsedUnrecognisedArg = checkForUnrecognisedArguments(argc, argv);

    if (hasUsedUnrecognisedArg) {
        // Print the help menu
        printHelp();
        return 0;
    }

    // Parse arguments
    parseArguments(argc, argv);

    // Check for contradictions
    if (checkForContradictions()) {
        return 1;
    }

    arg_t *verbosityArg = getArgumentByName("Verbosity");
    if (verbosityArg->intVal > 2)
    {
        LOG(LOG_DEBUG, "Verbosity set to %d, max is 2 - automatically lowering to 2", verbosityArg->intVal);
        verbosityArg->intVal = 3;
    }
    
    char *argList = malloc(sizeof(args));
    for (int i = 0; i < sizeof(args) / sizeof(arg_t); i++)
    {
        if (args[i].type == FLAG_BOOL && args[i].set)
        {
            sprintf(argList, "%s%s: %s, ", argList, args[i].name, args[i].boolVal ? "true" : "false");
        }
        else if (args[i].type == FLAG_INT && args[i].set)
        {
            sprintf(argList, "%s%s: %d, ", argList, args[i].name, args[i].intVal);
        }
        else if (args[i].type == FLAG_STRING && args[i].set) {
            sprintf(argList, "%s%s: %s, ", argList, args[i].name, args[i].stringVal);
        }
    }
    // Remove the trailing comma
    argList[strlen(argList) - 2] = '\0';

    LOG(LOG_DEBUG, "%s", argList);

    if (getArgumentByName("Help")->boolVal)
    {
        printHelp();
        return 0;
    }

#ifdef ACHILLES_LIBUSB
    char *usbBackend = "libusb";
#else
    char *usbBackend = "IOKit";
#endif

    struct utsname buffer;
    uname(&buffer);
    if (strcmp(buffer.sysname, "Darwin") != 0)
    {
        LOG(LOG_ERROR, "This tool is only supported on macOS");
        return -1;
    }
    LOG(LOG_DEBUG, "%s v%s, running on %s %s, %s", NAME, VERSION, buffer.sysname, buffer.machine, usbBackend);

    if (!getArgumentByName("Exploit")->set
    && !getArgumentByName("PongoOS")->set
    && !getArgumentByName("Jailbreak")->set) {
        LOG(LOG_ERROR, "Neither -e, -p nor -j were not passed, defaulting to exploit only");
        arg_t *exploitArg = getArgumentByName("Exploit");
        exploitArg->boolVal = true;
        exploitArg->set = true;
    }
    
    int i = 0;
    device_t device;
    while (findDevice(&device, false) == -1)
    {
        sleep(1);
        i++;
        if (i == 10)
        {
            LOG(LOG_ERROR, "No iOS device found after 10 seconds - please connect a device.");
            return -1;
        }
    }
    if (isSerialNumberPwned(device.serialNumber) && !isInDownloadMode(device.serialNumber))
    {
        LOG(LOG_DEBUG, "Serial number: %s", device.serialNumber);
        LOG(LOG_ERROR, "This device is already pwned.");
        return -1;
    }

    checkm8();

    return 0;
}
