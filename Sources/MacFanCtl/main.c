#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define SMC_SELECTOR 2
#define SMC_CMD_READ_BYTES 5
#define SMC_CMD_WRITE_BYTES 6
#define SMC_CMD_READ_KEYINFO 9

#define MAX_FANS 10
#define MAX_PARAMETERS 1024
#define MAX_ERRORS 256

typedef struct {
    uint8_t major;
    uint8_t minor;
    uint8_t build;
    uint8_t reserved;
    uint16_t release;
} SMCVersion;

typedef struct {
    uint16_t version;
    uint16_t length;
    uint32_t cpuPLimit;
    uint32_t gpuPLimit;
    uint32_t memPLimit;
} SMCPLimitData;

typedef struct {
    uint32_t dataSize;
    uint32_t dataType;
    uint8_t dataAttributes;
} SMCKeyInfoData;

typedef struct {
    uint32_t key;
    SMCVersion vers;
    SMCPLimitData pLimitData;
    SMCKeyInfoData keyInfo;
    uint8_t result;
    uint8_t status;
    uint8_t data8;
    uint32_t data32;
    uint8_t bytes[32];
} SMCParamStruct;

_Static_assert(sizeof(SMCParamStruct) == 80, "SMCParamStruct must be 80 bytes");

typedef struct {
    char key[5];
    char type[5];
    char raw[96];
    char value[128];
    char category[24];
    uint32_t size;
    bool numeric;
    double number;
} Parameter;

typedef struct {
    int index;
    bool present;
    bool actualSet;
    bool minSet;
    bool maxSet;
    bool targetSet;
    double actual;
    double min;
    double max;
    double target;
    bool manual;
    char name[64];
} Fan;

typedef struct {
    io_connect_t conn;
    bool opened;
    char serviceName[64];
    kern_return_t openResult;
    char openError[128];
} SMCConnection;

typedef struct {
    char platform[32];
    char strategy[64];
    bool supportedControl;
    int fanCount;
    int modeKeyCount;
    bool fsAvailable;
    bool ftstAvailable;
} HardwareProfile;

static Parameter parameters[MAX_PARAMETERS];
static int parameterCount = 0;
static char errors[MAX_ERRORS][160];
static int errorCount = 0;

static uint32_t strToKey(const char *key) {
    uint32_t result = 0;
    for (int i = 0; i < 4; i++) {
        result <<= 8;
        result |= (uint8_t)(key[i] ? key[i] : ' ');
    }
    return result;
}

static void keyToStr(uint32_t key, char out[5]) {
    out[0] = (char)((key >> 24) & 0xff);
    out[1] = (char)((key >> 16) & 0xff);
    out[2] = (char)((key >> 8) & 0xff);
    out[3] = (char)(key & 0xff);
    out[4] = 0;
}

static void typeToStr(uint32_t type, char out[5]) {
    keyToStr(type, out);
}

static void addError(const char *fmt, ...) {
    if (errorCount >= MAX_ERRORS) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(errors[errorCount], sizeof(errors[errorCount]), fmt, args);
    va_end(args);
    errorCount++;
}

static void jsonEscape(const char *s) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '\\': printf("\\\\"); break;
            case '"': printf("\\\""); break;
            case '\b': printf("\\b"); break;
            case '\f': printf("\\f"); break;
            case '\n': printf("\\n"); break;
            case '\r': printf("\\r"); break;
            case '\t': printf("\\t"); break;
            default:
                if (*p < 0x20) {
                    printf("\\u%04x", *p);
                } else {
                    putchar(*p);
                }
        }
    }
    putchar('"');
}

static void rawToHex(const uint8_t *bytes, uint32_t size, char *out, size_t outSize) {
    size_t offset = 0;
    if (outSize == 0) {
        return;
    }
    out[0] = 0;
    for (uint32_t i = 0; i < size && offset + 2 < outSize; i++) {
        int written = snprintf(out + offset, outSize - offset, "%02x", bytes[i]);
        if (written < 0) {
            break;
        }
        offset += (size_t)written;
    }
}

static bool getSysctlString(const char *name, char *buffer, size_t bufferSize) {
    size_t size = bufferSize;
    if (sysctlbyname(name, buffer, &size, NULL, 0) != 0 || size == 0) {
        snprintf(buffer, bufferSize, "unknown");
        return false;
    }
    buffer[bufferSize - 1] = 0;
    return true;
}

static SMCConnection openSMC(void) {
    SMCConnection smc;
    memset(&smc, 0, sizeof(smc));
    smc.conn = IO_OBJECT_NULL;
    smc.openResult = kIOReturnNotFound;

    const char *services[] = {"AppleSMC", "AppleSMCKeysEndpoint"};
    for (size_t i = 0; i < sizeof(services) / sizeof(services[0]); i++) {
        io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(services[i]));
        if (service == IO_OBJECT_NULL) {
            continue;
        }

        io_connect_t conn = IO_OBJECT_NULL;
        kern_return_t result = IOServiceOpen(service, mach_task_self(), 0, &conn);
        IOObjectRelease(service);
        smc.openResult = result;

        if (result == kIOReturnSuccess) {
            smc.conn = conn;
            smc.opened = true;
            snprintf(smc.serviceName, sizeof(smc.serviceName), "%s", services[i]);
            return smc;
        }
    }

    snprintf(smc.serviceName, sizeof(smc.serviceName), "not found");
    snprintf(smc.openError, sizeof(smc.openError), "IOServiceOpen failed: 0x%08x", smc.openResult);
    return smc;
}

static kern_return_t smcCall(SMCConnection *smc, SMCParamStruct *input, SMCParamStruct *output) {
    size_t outputSize = sizeof(SMCParamStruct);
    memset(output, 0, sizeof(SMCParamStruct));
    return IOConnectCallStructMethod(smc->conn,
                                     SMC_SELECTOR,
                                     input,
                                     sizeof(SMCParamStruct),
                                     output,
                                     &outputSize);
}

static kern_return_t readKeyInfo(SMCConnection *smc, const char *key, SMCKeyInfoData *info) {
    SMCParamStruct input;
    SMCParamStruct output;
    memset(&input, 0, sizeof(input));
    input.key = strToKey(key);
    input.data8 = SMC_CMD_READ_KEYINFO;

    kern_return_t result = smcCall(smc, &input, &output);
    if (result != kIOReturnSuccess) {
        return result;
    }
    if (output.result != 0) {
        return kIOReturnError;
    }

    *info = output.keyInfo;
    return kIOReturnSuccess;
}

static kern_return_t readKey(SMCConnection *smc, const char *key, SMCKeyInfoData *info, uint8_t bytes[32]) {
    kern_return_t result = readKeyInfo(smc, key, info);
    if (result != kIOReturnSuccess) {
        return result;
    }
    if (info->dataSize > 32) {
        return kIOReturnBadArgument;
    }

    SMCParamStruct input;
    SMCParamStruct output;
    memset(&input, 0, sizeof(input));
    input.key = strToKey(key);
    input.keyInfo.dataSize = info->dataSize;
    input.data8 = SMC_CMD_READ_BYTES;

    result = smcCall(smc, &input, &output);
    if (result != kIOReturnSuccess) {
        return result;
    }
    if (output.result != 0) {
        return kIOReturnError;
    }

    memset(bytes, 0, 32);
    memcpy(bytes, output.bytes, info->dataSize);
    return kIOReturnSuccess;
}

static kern_return_t writeKey(SMCConnection *smc, const char *key, const uint8_t *bytes, uint32_t size) {
    if (size > 32) {
        return kIOReturnBadArgument;
    }

    SMCKeyInfoData info;
    kern_return_t result = readKeyInfo(smc, key, &info);
    if (result != kIOReturnSuccess) {
        return result;
    }
    if (info.dataSize != size) {
        return kIOReturnBadArgument;
    }

    SMCParamStruct input;
    SMCParamStruct output;
    memset(&input, 0, sizeof(input));
    input.key = strToKey(key);
    input.keyInfo.dataSize = size;
    input.data8 = SMC_CMD_WRITE_BYTES;
    memcpy(input.bytes, bytes, size);

    result = smcCall(smc, &input, &output);
    if (result != kIOReturnSuccess) {
        return result;
    }
    if (output.result != 0) {
        return kIOReturnError;
    }
    return kIOReturnSuccess;
}

static double decodeFPE2(const uint8_t *bytes) {
    uint16_t raw = ((uint16_t)bytes[0] << 8) | bytes[1];
    return (double)raw / 4.0;
}

static double decodeSP78(const uint8_t *bytes) {
    int16_t raw = (int16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
    return (double)raw / 256.0;
}

static double decodeUI(const uint8_t *bytes, uint32_t size) {
    uint32_t value = 0;
    for (uint32_t i = 0; i < size && i < 4; i++) {
        value <<= 8;
        value |= bytes[i];
    }
    return (double)value;
}

static double decodeFLT(const uint8_t *bytes) {
    float value = 0;
    memcpy(&value, bytes, sizeof(value));
    return (double)value;
}

static uint16_t encodeFPE2(double value) {
    if (value < 0) {
        value = 0;
    }
    if (value > 16383.75) {
        value = 16383.75;
    }
    return (uint16_t)lrint(value * 4.0);
}

static bool decodeValue(const char type[5], const uint8_t *bytes, uint32_t size, char *out, size_t outSize, double *number) {
    *number = 0;

    if (strncmp(type, "fpe2", 4) == 0 && size >= 2) {
        *number = decodeFPE2(bytes);
        snprintf(out, outSize, "%.0f", *number);
        return true;
    }
    if (strncmp(type, "sp78", 4) == 0 && size >= 2) {
        *number = decodeSP78(bytes);
        snprintf(out, outSize, "%.1f", *number);
        return true;
    }
    if ((strncmp(type, "ui8 ", 4) == 0 || strncmp(type, "ui16", 4) == 0 || strncmp(type, "ui32", 4) == 0) && size >= 1) {
        *number = decodeUI(bytes, size);
        snprintf(out, outSize, "%.0f", *number);
        return true;
    }
    if (strncmp(type, "flag", 4) == 0 && size >= 1) {
        *number = bytes[0] ? 1 : 0;
        snprintf(out, outSize, "%s", bytes[0] ? "true" : "false");
        return true;
    }
    if (strncmp(type, "flt ", 4) == 0 && size >= 4) {
        *number = decodeFLT(bytes);
        snprintf(out, outSize, "%.2f", *number);
        return true;
    }
    if (strncmp(type, "ch8*", 4) == 0 || strncmp(type, "char", 4) == 0) {
        size_t n = size < outSize - 1 ? size : outSize - 1;
        for (size_t i = 0; i < n; i++) {
            unsigned char c = bytes[i];
            out[i] = (c >= 32 && c <= 126) ? (char)c : '.';
        }
        out[n] = 0;
        return false;
    }

    rawToHex(bytes, size, out, outSize);
    return false;
}

static Parameter *recordKey(SMCConnection *smc, const char *key, const char *category, bool quiet) {
    if (parameterCount >= MAX_PARAMETERS) {
        return NULL;
    }

    SMCKeyInfoData info;
    uint8_t bytes[32];
    kern_return_t result = readKey(smc, key, &info, bytes);
    if (result != kIOReturnSuccess) {
        if (!quiet) {
            addError("%s read failed: 0x%08x", key, result);
        }
        return NULL;
    }

    Parameter *parameter = &parameters[parameterCount++];
    memset(parameter, 0, sizeof(*parameter));
    snprintf(parameter->key, sizeof(parameter->key), "%s", key);
    typeToStr(info.dataType, parameter->type);
    parameter->size = info.dataSize;
    rawToHex(bytes, info.dataSize, parameter->raw, sizeof(parameter->raw));
    snprintf(parameter->category, sizeof(parameter->category), "%s", category);
    parameter->numeric = decodeValue(parameter->type, bytes, info.dataSize, parameter->value, sizeof(parameter->value), &parameter->number);
    return parameter;
}

static Parameter *findParameter(const char *key) {
    for (int i = 0; i < parameterCount; i++) {
        if (strncmp(parameters[i].key, key, 4) == 0) {
            return &parameters[i];
        }
    }
    return NULL;
}

static void readCandidateKeys(SMCConnection *smc);

static void resetReadings(void) {
    parameterCount = 0;
}

static void refreshReadings(SMCConnection *smc) {
    resetReadings();
    readCandidateKeys(smc);
}

static void readCandidateKeys(SMCConnection *smc) {
    const char *baseFanKeys[] = {"FNum", "FS! ", "Ftst"};
    const char *tempKeys[] = {
        "TC0P", "TC0E", "TC0F", "TC0D", "TC0H", "TC0p", "TC1C", "TC2C",
        "TG0P", "TG0D", "TG1D", "TG0H", "TB0T", "TB1T", "TB2T", "TB3T",
        "Tm0P", "Tm1P", "Ts0P", "Ts1P", "Tp0P", "Tp1P", "Ta0P", "Th0H",
        "TW0P", "TW1P", "TN0P", "TN1P", "TI0P", "TI1P",
    };
    const char *otherKeys[] = {
        "BNum", "B0AV", "B0AC", "B0FC", "B0RM", "B0St", "CH0B", "CH0C",
        "PC0C", "PC0R", "PCPC", "PCPG", "PCPT", "PSTR", "MSTc", "MSLD",
        "CLKT", "REV ", "RVBF", "RBr ", "RPlt", "RNum",
    };

    for (size_t i = 0; i < sizeof(baseFanKeys) / sizeof(baseFanKeys[0]); i++) {
        recordKey(smc, baseFanKeys[i], "fan", true);
    }

    int fanLimit = MAX_FANS;
    Parameter *fnum = findParameter("FNum");
    if (fnum && fnum->numeric && fnum->number > 0 && fnum->number < MAX_FANS) {
        fanLimit = (int)fnum->number;
    }

    const char *suffixes[] = {"Ac", "Mn", "Mx", "Sf", "Tg", "ID", "md", "Md"};
    for (int fan = 0; fan < fanLimit; fan++) {
        for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
            char key[5];
            snprintf(key, sizeof(key), "F%d%s", fan, suffixes[i]);
            recordKey(smc, key, "fan", true);
        }
    }
    for (size_t i = 0; i < sizeof(tempKeys) / sizeof(tempKeys[0]); i++) {
        recordKey(smc, tempKeys[i], "temperature", true);
    }
    for (size_t i = 0; i < sizeof(otherKeys) / sizeof(otherKeys[0]); i++) {
        recordKey(smc, otherKeys[i], "other", true);
    }
}

static Parameter *findFanModeParameter(int index) {
    char key[5];
    snprintf(key, sizeof(key), "F%dmd", index);
    Parameter *lower = findParameter(key);
    if (lower) {
        return lower;
    }

    snprintf(key, sizeof(key), "F%dMd", index);
    Parameter *upper = findParameter(key);
    if (upper) {
        return upper;
    }

    return NULL;
}

static bool firstFanModeKey(SMCConnection *smc, int index, char outKey[5]) {
    const char *templates[] = {"F%dmd", "F%dMd"};
    for (size_t i = 0; i < sizeof(templates) / sizeof(templates[0]); i++) {
        snprintf(outKey, 5, templates[i], index);
        SMCKeyInfoData info;
        if (readKeyInfo(smc, outKey, &info) == kIOReturnSuccess && info.dataSize > 0) {
            return true;
        }
    }
    outKey[0] = 0;
    return false;
}

static int buildFans(Fan fans[MAX_FANS]) {
    memset(fans, 0, sizeof(Fan) * MAX_FANS);

    int fanCount = 0;
    Parameter *fnum = findParameter("FNum");
    if (fnum && fnum->numeric) {
        fanCount = (int)fnum->number;
    }
    if (fanCount < 0) {
        fanCount = 0;
    }
    if (fanCount > MAX_FANS) {
        fanCount = MAX_FANS;
    }

    Parameter *fs = findParameter("FS! ");
    uint32_t manualMask = 0;
    if (fs) {
        if (fs->numeric) {
            manualMask = (uint32_t)fs->number;
        } else {
            sscanf(fs->raw, "%x", &manualMask);
        }
    }

    for (int i = 0; i < MAX_FANS; i++) {
        char key[5];
        Fan *fan = &fans[i];
        fan->index = i;
        snprintf(fan->name, sizeof(fan->name), "Fan %d", i);

        snprintf(key, sizeof(key), "F%dAc", i);
        Parameter *actual = findParameter(key);
        if (actual && actual->numeric) {
            fan->actual = actual->number;
            fan->actualSet = true;
            fan->present = true;
        }

        snprintf(key, sizeof(key), "F%dMn", i);
        Parameter *min = findParameter(key);
        if (min && min->numeric) {
            fan->min = min->number;
            fan->minSet = true;
            fan->present = true;
        }

        snprintf(key, sizeof(key), "F%dMx", i);
        Parameter *max = findParameter(key);
        if (max && max->numeric) {
            fan->max = max->number;
            fan->maxSet = true;
            fan->present = true;
        }

        snprintf(key, sizeof(key), "F%dTg", i);
        Parameter *target = findParameter(key);
        if (target && target->numeric) {
            fan->target = target->number;
            fan->targetSet = true;
            fan->present = true;
        }

        snprintf(key, sizeof(key), "F%dID", i);
        Parameter *name = findParameter(key);
        if (name && name->value[0]) {
            snprintf(fan->name, sizeof(fan->name), "%s", name->value);
        }

        fan->manual = (manualMask & (1u << i)) != 0;
        Parameter *mode = findFanModeParameter(i);
        if (mode && mode->numeric && mode->number != 0) {
            fan->manual = true;
        }

        if (fan->present && i + 1 > fanCount) {
            fanCount = i + 1;
        }
    }
    return fanCount;
}

static kern_return_t writeFS(SMCConnection *smc, uint16_t mask) {
    uint8_t bytes[2];
    bytes[0] = (uint8_t)((mask >> 8) & 0xff);
    bytes[1] = (uint8_t)(mask & 0xff);
    return writeKey(smc, "FS! ", bytes, 2);
}

static kern_return_t writeKeyPadded(SMCConnection *smc, const char *key, const uint8_t *bytes, uint32_t byteCount) {
    SMCKeyInfoData info;
    kern_return_t result = readKeyInfo(smc, key, &info);
    if (result != kIOReturnSuccess) {
        return result;
    }
    if (info.dataSize > 32) {
        return kIOReturnBadArgument;
    }

    SMCParamStruct input;
    SMCParamStruct output;
    memset(&input, 0, sizeof(input));
    input.key = strToKey(key);
    input.keyInfo.dataSize = info.dataSize;
    input.data8 = SMC_CMD_WRITE_BYTES;
    uint32_t copyCount = byteCount < info.dataSize ? byteCount : info.dataSize;
    memcpy(input.bytes, bytes, copyCount);

    result = smcCall(smc, &input, &output);
    if (result != kIOReturnSuccess) {
        return result;
    }
    if (output.result != 0) {
        return kIOReturnError;
    }
    return kIOReturnSuccess;
}

static kern_return_t writeByteKey(SMCConnection *smc, const char *key, uint8_t value) {
    uint8_t bytes[1] = {value};
    return writeKeyPadded(smc, key, bytes, 1);
}

static kern_return_t writeFanMode(SMCConnection *smc, int index, bool manual) {
    char key[5];
    if (!firstFanModeKey(smc, index, key)) {
        return kIOReturnNotFound;
    }
    return writeByteKey(smc, key, manual ? 1 : 0);
}

static bool hasFtst(SMCConnection *smc) {
    SMCKeyInfoData info;
    return readKeyInfo(smc, "Ftst", &info) == kIOReturnSuccess && info.dataSize > 0;
}

static HardwareProfile detectHardwareProfile(const char *machine) {
    HardwareProfile profile;
    memset(&profile, 0, sizeof(profile));

    if (strcmp(machine, "arm64") == 0) {
        snprintf(profile.platform, sizeof(profile.platform), "Apple Silicon");
    } else if (strcmp(machine, "x86_64") == 0) {
        snprintf(profile.platform, sizeof(profile.platform), "Intel");
    } else {
        snprintf(profile.platform, sizeof(profile.platform), "%s", machine);
    }

    Fan fans[MAX_FANS];
    profile.fanCount = buildFans(fans);
    profile.fsAvailable = findParameter("FS! ") != NULL;
    profile.ftstAvailable = findParameter("Ftst") != NULL;

    for (int i = 0; i < profile.fanCount; i++) {
        if (findFanModeParameter(i) != NULL) {
            profile.modeKeyCount++;
        }
    }

    if (profile.fanCount <= 0) {
        snprintf(profile.strategy, sizeof(profile.strategy), "no fan detected");
        profile.supportedControl = false;
    } else if (profile.modeKeyCount >= profile.fanCount) {
        snprintf(profile.strategy, sizeof(profile.strategy), "per-fan mode keys");
        profile.supportedControl = true;
    } else if (profile.modeKeyCount > 0 && profile.fsAvailable) {
        snprintf(profile.strategy, sizeof(profile.strategy), "mixed per-fan mode + legacy FS!");
        profile.supportedControl = true;
    } else if (profile.fsAvailable) {
        snprintf(profile.strategy, sizeof(profile.strategy), "legacy FS! mask");
        profile.supportedControl = true;
    } else if (profile.modeKeyCount > 0) {
        snprintf(profile.strategy, sizeof(profile.strategy), "partial per-fan mode keys");
        profile.supportedControl = false;
    } else {
        snprintf(profile.strategy, sizeof(profile.strategy), "read-only sensors");
        profile.supportedControl = false;
    }

    return profile;
}

static kern_return_t enableFanManualMode(SMCConnection *smc, int index) {
    kern_return_t direct = writeFanMode(smc, index, true);
    if (direct == kIOReturnSuccess) {
        return kIOReturnSuccess;
    }

    if (!hasFtst(smc)) {
        return direct;
    }

    kern_return_t ftst = writeByteKey(smc, "Ftst", 1);
    if (ftst != kIOReturnSuccess) {
        return ftst;
    }

    usleep(500000);
    kern_return_t last = kIOReturnError;
    for (int attempt = 0; attempt < 100; attempt++) {
        last = writeFanMode(smc, index, true);
        if (last == kIOReturnSuccess) {
            return kIOReturnSuccess;
        }
        usleep(100000);
    }

    return last;
}

static kern_return_t writeFanTarget(SMCConnection *smc, int index, double rpm) {
    char key[5];
    snprintf(key, sizeof(key), "F%dTg", index);

    SMCKeyInfoData info;
    kern_return_t result = readKeyInfo(smc, key, &info);
    if (result != kIOReturnSuccess) {
        return result;
    }

    char type[5];
    typeToStr(info.dataType, type);
    uint8_t bytes[32];
    memset(bytes, 0, sizeof(bytes));

    if (strncmp(type, "flt ", 4) == 0 && info.dataSize == 4) {
        float value = (float)rpm;
        memcpy(bytes, &value, sizeof(value));
        return writeKey(smc, key, bytes, 4);
    }

    if (strncmp(type, "fpe2", 4) == 0 && info.dataSize == 2) {
        uint16_t encoded = encodeFPE2(rpm);
        bytes[0] = (uint8_t)((encoded >> 8) & 0xff);
        bytes[1] = (uint8_t)(encoded & 0xff);
        return writeKey(smc, key, bytes, 2);
    }

    if (strncmp(type, "ui16", 4) == 0 && info.dataSize == 2) {
        uint16_t value = (uint16_t)lrint(rpm);
        bytes[0] = (uint8_t)((value >> 8) & 0xff);
        bytes[1] = (uint8_t)(value & 0xff);
        return writeKey(smc, key, bytes, 2);
    }

    return kIOReturnUnsupported;
}

static int setAuto(SMCConnection *smc) {
    refreshReadings(smc);

    Fan fans[MAX_FANS];
    int fanCount = buildFans(fans);
    int attempts = 0;
    int successes = 0;
    bool alreadyAuto = true;

    for (int i = 0; i < fanCount; i++) {
        if (fans[i].manual) {
            alreadyAuto = false;
        }

        char modeKey[5];
        if (!firstFanModeKey(smc, i, modeKey)) {
            continue;
        }

        attempts++;
        kern_return_t modeResult = writeFanMode(smc, i, false);
        if (modeResult == kIOReturnSuccess) {
            successes++;
        }
    }

    if (hasFtst(smc)) {
        attempts++;
        kern_return_t ftstResult = writeByteKey(smc, "Ftst", 0);
        if (ftstResult == kIOReturnSuccess) {
            successes++;
        }
    }

    kern_return_t fsResult = writeFS(smc, 0);
    if (fsResult == kIOReturnSuccess) {
        attempts++;
        successes++;
    }

    if (successes > 0 || alreadyAuto) {
        return 0;
    }

    if (attempts == 0) {
        addError("AUTO write skipped: no supported manual-mode key was found.");
    } else {
        addError("AUTO write failed: no supported manual-mode key accepted the write.");
    }
    if (fsResult != kIOReturnSuccess) {
        addError("Legacy FS! AUTO write failed: 0x%08x", fsResult);
    }
    return 1;
}

static int applyFanTargets(SMCConnection *smc, Fan fans[MAX_FANS], int fanCount, double rpmTargets[MAX_FANS]) {
    uint16_t legacyMask = 0;
    bool usedPerFanMode = false;
    bool needsLegacyMode = false;

    for (int i = 0; i < fanCount; i++) {
        Fan *fan = &fans[i];
        if (!fan->present) {
            continue;
        }

        char modeKey[5];
        if (firstFanModeKey(smc, i, modeKey)) {
            kern_return_t modeResult = enableFanManualMode(smc, i);
            if (modeResult != kIOReturnSuccess) {
                addError("Fan %d manual mode write failed via %s: 0x%08x", i, modeKey, modeResult);
                return 1;
            }
            usedPerFanMode = true;
        } else {
            needsLegacyMode = true;
            legacyMask |= (uint16_t)(1u << i);
        }

        kern_return_t targetResult = writeFanTarget(smc, i, rpmTargets[i]);
        if (targetResult != kIOReturnSuccess) {
            addError("F%dTg write failed: 0x%08x", i, targetResult);
            return 1;
        }
    }

    if (needsLegacyMode && !usedPerFanMode) {
        kern_return_t fsResult = writeFS(smc, legacyMask);
        if (fsResult != kIOReturnSuccess) {
            addError("Legacy FS! manual mode write failed: 0x%08x", fsResult);
            return 1;
        }
    } else if (needsLegacyMode) {
        kern_return_t fsResult = writeFS(smc, legacyMask);
        if (fsResult != kIOReturnSuccess) {
            addError("Some fans have no per-fan mode key, and legacy FS! failed: 0x%08x", fsResult);
            return 1;
        }
    }

    return 0;
}

static int prepareFans(SMCConnection *smc, Fan fans[MAX_FANS], int *fanCount) {
    refreshReadings(smc);
    *fanCount = buildFans(fans);
    if (*fanCount == 0) {
        addError("No controllable fans were reported by SMC.");
        return 1;
    }
    return 0;
}

static int setPercent(SMCConnection *smc, double percent) {
    if (percent <= 0.0) {
        return setAuto(smc);
    }
    if (percent > 100.0) {
        percent = 100.0;
    }

    errorCount = 0;

    Fan fans[MAX_FANS];
    int fanCount = 0;
    if (prepareFans(smc, fans, &fanCount) != 0) {
        return 1;
    }

    double rpmTargets[MAX_FANS];
    memset(rpmTargets, 0, sizeof(rpmTargets));
    for (int i = 0; i < fanCount; i++) {
        Fan *fan = &fans[i];
        if (!fan->present) {
            continue;
        }
        if (!fan->minSet || !fan->maxSet || fan->max <= fan->min) {
            addError("Fan %d is missing min/max RPM; refusing to guess target.", i);
            return 1;
        }
        rpmTargets[i] = fan->min + ((fan->max - fan->min) * (percent / 100.0));
    }

    return applyFanTargets(smc, fans, fanCount, rpmTargets);
}

static int setRPM(SMCConnection *smc, double rpm) {
    if (rpm <= 0.0) {
        return setAuto(smc);
    }

    errorCount = 0;

    Fan fans[MAX_FANS];
    int fanCount = 0;
    if (prepareFans(smc, fans, &fanCount) != 0) {
        return 1;
    }

    double rpmTargets[MAX_FANS];
    memset(rpmTargets, 0, sizeof(rpmTargets));
    for (int i = 0; i < fanCount; i++) {
        Fan *fan = &fans[i];
        if (!fan->present) {
            continue;
        }
        double target = rpm;
        if (fan->minSet && target < fan->min) {
            target = fan->min;
        }
        if (fan->maxSet && target > fan->max) {
            target = fan->max;
        }
        rpmTargets[i] = target;
    }

    return applyFanTargets(smc, fans, fanCount, rpmTargets);
}

static void printJSON(SMCConnection *smc, const char *command, int exitCode) {
    char model[128];
    char machine[128];
    char osVersion[128];
    getSysctlString("hw.model", model, sizeof(model));
    getSysctlString("hw.machine", machine, sizeof(machine));
    getSysctlString("kern.osproductversion", osVersion, sizeof(osVersion));

    Fan fans[MAX_FANS];
    int fanCount = buildFans(fans);
    HardwareProfile profile = detectHardwareProfile(machine);

    printf("{");
    printf("\"ok\":%s,", (exitCode == 0 && smc->opened && errorCount == 0) ? "true" : "false");
    printf("\"command\":");
    jsonEscape(command);
    printf(",");
    printf("\"effectiveUserId\":%d,", geteuid());
    printf("\"system\":{");
    printf("\"model\":"); jsonEscape(model); printf(",");
    printf("\"machine\":"); jsonEscape(machine); printf(",");
    printf("\"osVersion\":"); jsonEscape(osVersion);
    printf("},");
    printf("\"smc\":{");
    printf("\"opened\":%s,", smc->opened ? "true" : "false");
    printf("\"service\":"); jsonEscape(smc->serviceName[0] ? smc->serviceName : "unknown"); printf(",");
    printf("\"openResult\":\"0x%08x\"", smc->openResult);
    if (smc->openError[0]) {
        printf(",\"error\":");
        jsonEscape(smc->openError);
    }
    printf("},");

    printf("\"hardwareProfile\":{");
    printf("\"modelIdentifier\":"); jsonEscape(model); printf(",");
    printf("\"architecture\":"); jsonEscape(machine); printf(",");
    printf("\"platform\":"); jsonEscape(profile.platform); printf(",");
    printf("\"controlStrategy\":"); jsonEscape(profile.strategy); printf(",");
    printf("\"supportedControl\":%s,", profile.supportedControl ? "true" : "false");
    printf("\"fanCount\":%d,", profile.fanCount);
    printf("\"modeKeyCount\":%d,", profile.modeKeyCount);
    printf("\"legacyFSAvailable\":%s,", profile.fsAvailable ? "true" : "false");
    printf("\"ftstAvailable\":%s,", profile.ftstAvailable ? "true" : "false");
    printf("\"fanKeyLimit\":%d", MAX_FANS);
    printf("},");

    printf("\"fans\":[");
    bool firstFan = true;
    for (int i = 0; i < fanCount; i++) {
        Fan *fan = &fans[i];
        if (!fan->present) {
            continue;
        }
        if (!firstFan) {
            printf(",");
        }
        firstFan = false;
        printf("{\"index\":%d,", fan->index);
        printf("\"name\":"); jsonEscape(fan->name); printf(",");
        printf("\"manual\":%s,", fan->manual ? "true" : "false");
        printf("\"actualRpm\":"); fan->actualSet ? printf("%.0f", fan->actual) : printf("null"); printf(",");
        printf("\"minRpm\":"); fan->minSet ? printf("%.0f", fan->min) : printf("null"); printf(",");
        printf("\"maxRpm\":"); fan->maxSet ? printf("%.0f", fan->max) : printf("null"); printf(",");
        printf("\"targetRpm\":"); fan->targetSet ? printf("%.0f", fan->target) : printf("null");
        printf("}");
    }
    printf("],");

    printf("\"parameters\":[");
    for (int i = 0; i < parameterCount; i++) {
        Parameter *p = &parameters[i];
        if (i > 0) {
            printf(",");
        }
        printf("{\"key\":"); jsonEscape(p->key); printf(",");
        printf("\"type\":"); jsonEscape(p->type); printf(",");
        printf("\"size\":%u,", p->size);
        printf("\"category\":"); jsonEscape(p->category); printf(",");
        printf("\"value\":"); jsonEscape(p->value); printf(",");
        printf("\"raw\":"); jsonEscape(p->raw);
        if (p->numeric) {
            printf(",\"number\":%.3f", p->number);
        }
        printf("}");
    }
    printf("],");

    printf("\"errors\":[");
    for (int i = 0; i < errorCount; i++) {
        if (i > 0) {
            printf(",");
        }
        jsonEscape(errors[i]);
    }
    printf("]");
    printf("}\n");
}

static void usage(void) {
    fprintf(stderr, "usage: macfanctl status | auto | set-percent 0..100 | set-rpm RPM | max\n");
}

int main(int argc, char **argv) {
    const char *command = argc > 1 ? argv[1] : "status";
    int exitCode = 0;

    SMCConnection smc = openSMC();
    if (!smc.opened) {
        addError("%s", smc.openError[0] ? smc.openError : "Unable to open SMC service.");
        printJSON(&smc, command, 1);
        return 1;
    }

    if (strcmp(command, "status") == 0) {
        refreshReadings(&smc);
    } else if ((strcmp(command, "auto") == 0 ||
                strcmp(command, "set-percent") == 0 ||
                strcmp(command, "set-rpm") == 0 ||
                strcmp(command, "max") == 0) && geteuid() != 0) {
        addError("Write commands require the pkg-installed setuid helper at /Library/PrivilegedHelperTools/com.codex.macfanctl.");
        exitCode = 1;
        refreshReadings(&smc);
    } else if (strcmp(command, "auto") == 0) {
        exitCode = setAuto(&smc);
        if (exitCode == 0) {
            usleep(600000);
        }
        refreshReadings(&smc);
    } else if (strcmp(command, "set-percent") == 0) {
        if (argc < 3) {
            addError("Missing percent value.");
            exitCode = 1;
        } else {
            char *end = NULL;
            errno = 0;
            double percent = strtod(argv[2], &end);
            if (errno != 0 || end == argv[2] || percent < 0.0 || percent > 100.0) {
                addError("Percent must be a number from 0 to 100.");
                exitCode = 1;
            } else {
                exitCode = setPercent(&smc, percent);
            }
        }
        if (exitCode == 0) {
            usleep(600000);
        }
        refreshReadings(&smc);
    } else if (strcmp(command, "set-rpm") == 0) {
        if (argc < 3) {
            addError("Missing RPM value.");
            exitCode = 1;
        } else {
            char *end = NULL;
            errno = 0;
            double rpm = strtod(argv[2], &end);
            if (errno != 0 || end == argv[2] || rpm < 0.0) {
                addError("RPM must be a non-negative number.");
                exitCode = 1;
            } else {
                exitCode = setRPM(&smc, rpm);
            }
        }
        if (exitCode == 0) {
            usleep(600000);
        }
        refreshReadings(&smc);
    } else if (strcmp(command, "max") == 0) {
        exitCode = setPercent(&smc, 100.0);
        if (exitCode == 0) {
            usleep(600000);
        }
        refreshReadings(&smc);
    } else {
        usage();
        addError("Unknown command: %s", command);
        exitCode = 2;
        refreshReadings(&smc);
    }

    printJSON(&smc, command, exitCode);
    if (smc.opened) {
        IOServiceClose(smc.conn);
    }
    return exitCode;
}
