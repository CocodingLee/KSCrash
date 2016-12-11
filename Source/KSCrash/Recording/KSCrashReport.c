//
//  KSCrashReport.m
//
//  Created by Karl Stenerud on 2012-01-28.
//
//  Copyright (c) 2012 Karl Stenerud. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall remain in place
// in this source code.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//


#include "KSCrashReport.h"

#include "KSBacktrace.h"
#include "KSCrashReportFields.h"
#include "KSCrashReportWriter.h"
#include "KSDynamicLinker.h"
#include "KSFileUtils.h"
#include "KSJSONCodec.h"
#include "KSCPU.h"
#include "KSMemory.h"
#include "KSReturnCodes.h"
#include "KSThread.h"
#include "KSObjC.h"
#include "KSSignalInfo.h"
#include "KSCrashMonitor_Zombie.h"
#include "KSString.h"
#include "KSCrashReportVersion.h"

//#define KSLogger_LocalLevel TRACE
#include "KSLogger.h"

#include <errno.h>
#include <fcntl.h>
#include <mach/exception_types.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


#ifdef __arm64__
    #include <sys/_types/_ucontext64.h>
    #define UC_MCONTEXT uc_mcontext64
    typedef ucontext64_t SignalUserContext;
#else
    #define UC_MCONTEXT uc_mcontext
    typedef ucontext_t SignalUserContext;
#endif


// ============================================================================
#pragma mark - Constants -
// ============================================================================

/** Maximum depth allowed for a backtrace. */
#define kMaxBacktraceDepth 150

/** Default number of objects, subobjects, and ivars to record from a memory loc */
#define kDefaultMemorySearchDepth 15

/** Maximum number of lines to print when printing a stack trace to the console.
 */
#define kMaxStackTracePrintLines 40

/** How far to search the stack (in pointer sized jumps) for notable data. */
#define kStackNotableSearchBackDistance 20
#define kStackNotableSearchForwardDistance 10

/** How much of the stack to dump (in pointer sized jumps). */
#define kStackContentsPushedDistance 20
#define kStackContentsPoppedDistance 10
#define kStackContentsTotalDistance (kStackContentsPushedDistance + kStackContentsPoppedDistance)

/** The minimum length for a valid string. */
#define kMinStringLength 4


// ============================================================================
#pragma mark - Formatting -
// ============================================================================

#if defined(__LP64__)
    #define TRACE_FMT         "%-4d%-31s 0x%016lx %s + %lu"
    #define POINTER_FMT       "0x%016lx"
    #define POINTER_SHORT_FMT "0x%lx"
#else
    #define TRACE_FMT         "%-4d%-31s 0x%08lx %s + %lu"
    #define POINTER_FMT       "0x%08lx"
    #define POINTER_SHORT_FMT "0x%lx"
#endif


// ============================================================================
#pragma mark - JSON Encoding -
// ============================================================================

#define getJsonContext(REPORT_WRITER) ((KSJSONEncodeContext*)((REPORT_WRITER)->context))

/** Used for writing hex string values. */
static const char g_hexNybbles[] =
{
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
};

// ============================================================================
#pragma mark - Runtime Config -
// ============================================================================

static KSCrash_IntrospectionRules* g_introspectionRules;


#pragma mark Callbacks

static void addBooleanElement(const KSCrashReportWriter* const writer, const char* const key, const bool value)
{
    ksjson_addBooleanElement(getJsonContext(writer), key, value);
}

static void addFloatingPointElement(const KSCrashReportWriter* const writer, const char* const key, const double value)
{
    ksjson_addFloatingPointElement(getJsonContext(writer), key, value);
}

static void addIntegerElement(const KSCrashReportWriter* const writer, const char* const key, const int64_t value)
{
    ksjson_addIntegerElement(getJsonContext(writer), key, value);
}

static void addUIntegerElement(const KSCrashReportWriter* const writer, const char* const key, const uint64_t value)
{
    ksjson_addIntegerElement(getJsonContext(writer), key, (int64_t)value);
}

static void addStringElement(const KSCrashReportWriter* const writer, const char* const key, const char* const value)
{
    ksjson_addStringElement(getJsonContext(writer), key, value, KSJSON_SIZE_AUTOMATIC);
}

static void addTextFileElement(const KSCrashReportWriter* const writer, const char* const key, const char* const filePath)
{
    const int fd = open(filePath, O_RDONLY);
    if(fd < 0)
    {
        KSLOG_ERROR("Could not open file %s: %s", filePath, strerror(errno));
        return;
    }

    if(ksjson_beginStringElement(getJsonContext(writer), key) != KSJSON_OK)
    {
        KSLOG_ERROR("Could not start string element");
        goto done;
    }

    char buffer[512];
    int bytesRead;
    for(bytesRead = (int)read(fd, buffer, sizeof(buffer));
        bytesRead > 0;
        bytesRead = (int)read(fd, buffer, sizeof(buffer)))
    {
        if(ksjson_appendStringElement(getJsonContext(writer), buffer, bytesRead) != KSJSON_OK)
        {
            KSLOG_ERROR("Could not append string element");
            goto done;
        }
    }

done:
    ksjson_endStringElement(getJsonContext(writer));
    close(fd);
}

static void addDataElement(const KSCrashReportWriter* const writer,
                           const char* const key,
                           const char* const value,
                           const int length)
{
    ksjson_addDataElement(getJsonContext(writer), key, value, length);
}

static void beginDataElement(const KSCrashReportWriter* const writer, const char* const key)
{
    ksjson_beginDataElement(getJsonContext(writer), key);
}

static void appendDataElement(const KSCrashReportWriter* const writer, const char* const value, const int length)
{
    ksjson_appendDataElement(getJsonContext(writer), value, length);
}

static void endDataElement(const KSCrashReportWriter* const writer)
{
    ksjson_endDataElement(getJsonContext(writer));
}

static void addUUIDElement(const KSCrashReportWriter* const writer, const char* const key, const unsigned char* const value)
{
    if(value == NULL)
    {
        ksjson_addNullElement(getJsonContext(writer), key);
    }
    else
    {
        char uuidBuffer[37];
        const unsigned char* src = value;
        char* dst = uuidBuffer;
        for(int i = 0; i < 4; i++)
        {
            *dst++ = g_hexNybbles[(*src>>4)&15];
            *dst++ = g_hexNybbles[(*src++)&15];
        }
        *dst++ = '-';
        for(int i = 0; i < 2; i++)
        {
            *dst++ = g_hexNybbles[(*src>>4)&15];
            *dst++ = g_hexNybbles[(*src++)&15];
        }
        *dst++ = '-';
        for(int i = 0; i < 2; i++)
        {
            *dst++ = g_hexNybbles[(*src>>4)&15];
            *dst++ = g_hexNybbles[(*src++)&15];
        }
        *dst++ = '-';
        for(int i = 0; i < 2; i++)
        {
            *dst++ = g_hexNybbles[(*src>>4)&15];
            *dst++ = g_hexNybbles[(*src++)&15];
        }
        *dst++ = '-';
        for(int i = 0; i < 6; i++)
        {
            *dst++ = g_hexNybbles[(*src>>4)&15];
            *dst++ = g_hexNybbles[(*src++)&15];
        }

        ksjson_addStringElement(getJsonContext(writer), key, uuidBuffer, (int)(dst - uuidBuffer));
    }
}

static void addJSONElement(const KSCrashReportWriter* const writer,
                           const char* const key,
                           const char* const jsonElement,
                           bool closeLastContainer)
{
    int jsonResult = ksjson_addJSONElement(getJsonContext(writer),
                                           key,
                                           jsonElement,
                                           (int)strlen(jsonElement),
                                           closeLastContainer);
    if(jsonResult != KSJSON_OK)
    {
        char errorBuff[100];
        snprintf(errorBuff,
                 sizeof(errorBuff),
                 "Invalid JSON data: %s",
                 ksjson_stringForError(jsonResult));
        ksjson_beginObject(getJsonContext(writer), key);
        ksjson_addStringElement(getJsonContext(writer),
                                KSCrashField_Error,
                                errorBuff,
                                KSJSON_SIZE_AUTOMATIC);
        ksjson_addStringElement(getJsonContext(writer),
                                KSCrashField_JSONData,
                                jsonElement,
                                KSJSON_SIZE_AUTOMATIC);
        ksjson_endContainer(getJsonContext(writer));
    }
}

static void addJSONElementFromFile(const KSCrashReportWriter* const writer,
                                   const char* const key,
                                   const char* const filePath,
                                   bool closeLastContainer)
{
    ksjson_addJSONFromFile(getJsonContext(writer), key, filePath, closeLastContainer);
}

static void beginObject(const KSCrashReportWriter* const writer, const char* const key)
{
    ksjson_beginObject(getJsonContext(writer), key);
}

static void beginArray(const KSCrashReportWriter* const writer, const char* const key)
{
    ksjson_beginArray(getJsonContext(writer), key);
}

static void endContainer(const KSCrashReportWriter* const writer)
{
    ksjson_endContainer(getJsonContext(writer));
}

typedef struct
{
    char buffer[1024];
    int length;
    int position;
    int fd;
} BufferedWriter;

static bool flushBufferedWriter(BufferedWriter* writer)
{
    if(writer->fd > 0 && writer->position > 0)
    {
        if(!ksfu_writeBytesToFD(writer->fd, writer->buffer, writer->position))
        {
            return false;
        }
        writer->position = 0;
    }
    return true;
}

static void closeBufferedWriter(BufferedWriter* writer)
{
    if(writer->fd > 0)
    {
        flushBufferedWriter(writer);
        close(writer->fd);
        writer->fd = -1;
    }
}

static bool openBufferedWriter(BufferedWriter* writer, const char* const path)
{
    writer->position = 0;
    writer->length = sizeof(writer->buffer);
    writer->fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
    if(writer->fd < 0)
    {
        KSLOG_ERROR("Could not open crash report file %s: %s", path, strerror(errno));
        return false;
    }
    return true;
}

static bool writeBufferedWriter(BufferedWriter* writer, const char* restrict const data, const int length)
{
    if(length > writer->length - writer->position)
    {
        flushBufferedWriter(writer);
    }
    if(length > writer->length)
    {
        return ksfu_writeBytesToFD(writer->fd, data, length);
    }
    memcpy(writer->buffer + writer->position, data, length);
    writer->position += length;
    return true;
}

static int addJSONData(const char* restrict const data, const int length, void* restrict userData)
{
    BufferedWriter* writer = (BufferedWriter*)userData;
    const bool success = writeBufferedWriter(writer, data, length);
    return success ? KSJSON_OK : KSJSON_ERROR_CANNOT_ADD_DATA;
}


// ============================================================================
#pragma mark - Utility -
// ============================================================================

/** Check if a memory address points to a valid null terminated UTF-8 string.
 *
 * @param address The address to check.
 *
 * @return true if the address points to a string.
 */
static bool isValidString(const void* const address)
{
    if((void*)address == NULL)
    {
        return false;
    }

    char buffer[500];
    if((uintptr_t)address+sizeof(buffer) < (uintptr_t)address)
    {
        // Wrapped around the address range.
        return false;
    }
    if(!ksmem_copySafely(address, buffer, sizeof(buffer)))
    {
        return false;
    }
    return ksstring_isNullTerminatedUTF8String(buffer, kMinStringLength, sizeof(buffer));
}

/** Get the backtrace for the specified machine context.
 *
 * This function will choose how to fetch the backtrace based on the crash and
 * machine context. It may store the backtrace in backtraceBuffer unless it can
 * be fetched directly from memory. Do not count on backtraceBuffer containing
 * anything. Always use the return value.
 *
 * @param crash The crash handler context.
 *
 * @param machineContext The machine context.
 *
 * @param backtraceBuffer A place to store the backtrace, if needed.
 *
 * @param backtraceLength In: The length of backtraceBuffer.
 *                        Out: The length of the backtrace.
 *
 * @param skippedEntries Out: The number of entries that were skipped due to
 *                             stack overflow.
 *
 * @return The backtrace, or NULL if not found.
 */
static uintptr_t* getBacktrace(const KSCrash_MonitorContext* const crash,
                               const KSMachineContext machineContext,
                               uintptr_t* const backtraceBuffer,
                               int* const backtraceLength,
                               int* const skippedEntries)
{
    if(ksmc_canHaveCustomStackTrace(machineContext) && crash->stackTrace != NULL && crash->stackTraceLength > 0)
    {
        *backtraceLength = crash->stackTraceLength;
        if(skippedEntries != NULL)
        {
            *skippedEntries = 0;
        }
        return crash->stackTrace;
    }

    if(ksmc_canHaveNormalStackTrace(machineContext))
    {
        int actualSkippedEntries = 0;
        int actualLength = ksbt_backtraceLength(machineContext);
        if(actualLength > *backtraceLength)
        {
            actualSkippedEntries = actualLength - *backtraceLength;
        }
        
        *backtraceLength = ksbt_backtrace(machineContext, backtraceBuffer, actualSkippedEntries, *backtraceLength);
        if(skippedEntries != NULL)
        {
            *skippedEntries = actualSkippedEntries;
        }
        return backtraceBuffer;
    }

    return NULL;
}


// ============================================================================
#pragma mark - Console Logging -
// ============================================================================

/** Print the crash type and location to the log.
 *
 * @param monitorContext The crash monitor context.
 */
static void logCrashType(const KSCrash_MonitorContext* const monitorContext)
{
    switch(monitorContext->crashType)
    {
        case KSCrashMonitorTypeMachException:
        {
            int machExceptionType = monitorContext->mach.type;
            kern_return_t machCode = (kern_return_t)monitorContext->mach.code;
            const char* machExceptionName = ksrc_exceptionName(machExceptionType);
            const char* machCodeName = machCode == 0 ? NULL : ksmemory_kernelReturnCodeName(machCode);
            KSLOGBASIC_INFO("App crashed due to mach exception: [%s: %s] at %p",
                            machExceptionName, machCodeName, monitorContext->faultAddress);
            break;
        }
        case KSCrashMonitorTypeCPPException:
        {
            KSLOG_INFO("App crashed due to C++ exception: %s: %s",
                       monitorContext->CPPException.name,
                       monitorContext->crashReason);
            break;
        }
        case KSCrashMonitorTypeNSException:
        {
            KSLOGBASIC_INFO("App crashed due to NSException: %s: %s",
                            monitorContext->NSException.name,
                            monitorContext->crashReason);
            break;
        }
        case KSCrashMonitorTypeSignal:
        {
            int sigNum = monitorContext->signal.signalInfo->si_signo;
            int sigCode = monitorContext->signal.signalInfo->si_code;
            const char* sigName = kssignal_signalName(sigNum);
            const char* sigCodeName = kssignal_signalCodeName(sigNum, sigCode);
            KSLOGBASIC_INFO("App crashed due to signal: [%s, %s] at %08x",
                            sigName, sigCodeName, monitorContext->faultAddress);
            break;
        }
        case KSCrashMonitorTypeMainThreadDeadlock:
        {
            KSLOGBASIC_INFO("Main thread deadlocked");
            break;
        }
        case KSCrashMonitorTypeUserReported:
        {
            KSLOG_INFO("App crashed due to user specified exception: %s", monitorContext->crashReason);
            break;
        }
    }
}

/** Print a backtrace entry in the standard format to the log.
 *
 * @param entryNum The backtrace entry number.
 *
 * @param address The program counter value (instruction address).
 *
 * @param dlInfo Information about the nearest symbols to the address.
 */
static void logBacktraceEntry(const int entryNum, const uintptr_t address, const Dl_info* const dlInfo)
{
    char faddrBuff[20];
    char saddrBuff[20];

    const char* fname = ksfu_lastPathEntry(dlInfo->dli_fname);
    if(fname == NULL)
    {
        sprintf(faddrBuff, POINTER_FMT, (uintptr_t)dlInfo->dli_fbase);
        fname = faddrBuff;
    }

    uintptr_t offset = address - (uintptr_t)dlInfo->dli_saddr;
    const char* sname = dlInfo->dli_sname;
    if(sname == NULL)
    {
        sprintf(saddrBuff, POINTER_SHORT_FMT, (uintptr_t)dlInfo->dli_fbase);
        sname = saddrBuff;
        offset = address - (uintptr_t)dlInfo->dli_fbase;
    }

    KSLOGBASIC_ALWAYS(TRACE_FMT, entryNum, fname, address, sname, offset);
}

/** Print a backtrace to the log.
 *
 * @param backtrace The backtrace to print.
 *
 * @param backtraceLength The length of the backtrace.
 */
static void logBacktrace(const uintptr_t* const backtrace, const int backtraceLength, const int skippedEntries)
{
    if(backtraceLength > 0)
    {
        Dl_info symbolicated[backtraceLength];
        ksbt_symbolicate(backtrace, symbolicated, backtraceLength, skippedEntries);

        for(int i = 0; i < backtraceLength; i++)
        {
            logBacktraceEntry(i, backtrace[i], &symbolicated[i]);
        }
    }
}

/** Print the backtrace for the crashed thread to the log.
 *
 * @param crash The crash handler context.
 */
static void logCrashThreadBacktrace(const KSCrash_MonitorContext* const crash)
{
    uintptr_t concreteBacktrace[kMaxStackTracePrintLines];
    int backtraceLength = sizeof(concreteBacktrace) / sizeof(*concreteBacktrace);

    int skippedEntries = 0;
    uintptr_t* backtrace = getBacktrace(crash,
                                        crash->offendingMachineContext,
                                        concreteBacktrace,
                                        &backtraceLength,
                                        &skippedEntries);

    if(backtrace != NULL)
    {
        logBacktrace(backtrace, backtraceLength, skippedEntries);
    }
}


// ============================================================================
#pragma mark - Report Writing -
// ============================================================================

/** Write the contents of a memory location.
 * Also writes meta information about the data.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param address The memory address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
static void writeMemoryContents(const KSCrashReportWriter* const writer,
                                const char* const key,
                                const uintptr_t address,
                                int* limit);

/** Write a string to the report.
 * This will only print the first child of the array.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param objectAddress The object's address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
static void writeNSStringContents(const KSCrashReportWriter* const writer,
                                  const char* const key,
                                  const uintptr_t objectAddress,
                                  __unused int* limit)
{
    const void* object = (const void*)objectAddress;
    char buffer[200];
    if(ksobjc_copyStringContents(object, buffer, sizeof(buffer)))
    {
        writer->addStringElement(writer, key, buffer);
    }
}

/** Write a URL to the report.
 * This will only print the first child of the array.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param objectAddress The object's address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
static void writeURLContents(const KSCrashReportWriter* const writer,
                             const char* const key,
                             const uintptr_t objectAddress,
                             __unused int* limit)
{
    const void* object = (const void*)objectAddress;
    char buffer[200];
    if(ksobjc_copyStringContents(object, buffer, sizeof(buffer)))
    {
        writer->addStringElement(writer, key, buffer);
    }
}

/** Write a date to the report.
 * This will only print the first child of the array.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param objectAddress The object's address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
static void writeDateContents(const KSCrashReportWriter* const writer,
                              const char* const key,
                              const uintptr_t objectAddress,
                              __unused int* limit)
{
    const void* object = (const void*)objectAddress;
    writer->addFloatingPointElement(writer, key, ksobjc_dateContents(object));
}

/** Write a number to the report.
 * This will only print the first child of the array.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param objectAddress The object's address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
static void writeNumberContents(const KSCrashReportWriter* const writer,
                                const char* const key,
                                const uintptr_t objectAddress,
                                __unused int* limit)
{
    const void* object = (const void*)objectAddress;
    writer->addFloatingPointElement(writer, key, ksobjc_numberAsFloat(object));
}

/** Write an array to the report.
 * This will only print the first child of the array.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param objectAddress The object's address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
static void writeArrayContents(const KSCrashReportWriter* const writer,
                               const char* const key,
                               const uintptr_t objectAddress,
                               int* limit)
{
    const void* object = (const void*)objectAddress;
    uintptr_t firstObject;
    if(ksobjc_arrayContents(object, &firstObject, 1) == 1)
    {
        writeMemoryContents(writer, key, firstObject, limit);
    }
}

/** Write out ivar information about an unknown object.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param objectAddress The object's address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
static void writeUnknownObjectContents(const KSCrashReportWriter* const writer,
                                       const char* const key,
                                       const uintptr_t objectAddress,
                                       int* limit)
{
    (*limit)--;
    const void* object = (const void*)objectAddress;
    KSObjCIvar ivars[10];
    int8_t s8;
    int16_t s16;
    int sInt;
    int32_t s32;
    int64_t s64;
    uint8_t u8;
    uint16_t u16;
    unsigned int uInt;
    uint32_t u32;
    uint64_t u64;
    float f32;
    double f64;
    bool b;
    void* pointer;
    
    
    writer->beginObject(writer, key);
    {
        if(ksobjc_isTaggedPointer(object))
        {
            writer->addIntegerElement(writer, "tagged_payload", (int64_t)ksobjc_taggedPointerPayload(object));
        }
        else
        {
            const void* class = ksobjc_isaPointer(object);
            int ivarCount = ksobjc_ivarList(class, ivars, sizeof(ivars)/sizeof(*ivars));
            *limit -= ivarCount;
            for(int i = 0; i < ivarCount; i++)
            {
                KSObjCIvar* ivar = &ivars[i];
                switch(ivar->type[0])
                {
                    case 'c':
                        ksobjc_ivarValue(object, ivar->index, &s8);
                        writer->addIntegerElement(writer, ivar->name, s8);
                        break;
                    case 'i':
                        ksobjc_ivarValue(object, ivar->index, &sInt);
                        writer->addIntegerElement(writer, ivar->name, sInt);
                        break;
                    case 's':
                        ksobjc_ivarValue(object, ivar->index, &s16);
                        writer->addIntegerElement(writer, ivar->name, s16);
                        break;
                    case 'l':
                        ksobjc_ivarValue(object, ivar->index, &s32);
                        writer->addIntegerElement(writer, ivar->name, s32);
                        break;
                    case 'q':
                        ksobjc_ivarValue(object, ivar->index, &s64);
                        writer->addIntegerElement(writer, ivar->name, s64);
                        break;
                    case 'C':
                        ksobjc_ivarValue(object, ivar->index, &u8);
                        writer->addUIntegerElement(writer, ivar->name, u8);
                        break;
                    case 'I':
                        ksobjc_ivarValue(object, ivar->index, &uInt);
                        writer->addUIntegerElement(writer, ivar->name, uInt);
                        break;
                    case 'S':
                        ksobjc_ivarValue(object, ivar->index, &u16);
                        writer->addUIntegerElement(writer, ivar->name, u16);
                        break;
                    case 'L':
                        ksobjc_ivarValue(object, ivar->index, &u32);
                        writer->addUIntegerElement(writer, ivar->name, u32);
                        break;
                    case 'Q':
                        ksobjc_ivarValue(object, ivar->index, &u64);
                        writer->addUIntegerElement(writer, ivar->name, u64);
                        break;
                    case 'f':
                        ksobjc_ivarValue(object, ivar->index, &f32);
                        writer->addFloatingPointElement(writer, ivar->name, f32);
                        break;
                    case 'd':
                        ksobjc_ivarValue(object, ivar->index, &f64);
                        writer->addFloatingPointElement(writer, ivar->name, f64);
                        break;
                    case 'B':
                        ksobjc_ivarValue(object, ivar->index, &b);
                        writer->addBooleanElement(writer, ivar->name, b);
                        break;
                    case '*':
                    case '@':
                    case '#':
                    case ':':
                        ksobjc_ivarValue(object, ivar->index, &pointer);
                        writeMemoryContents(writer, ivar->name, (uintptr_t)pointer, limit);
                        break;
                    default:
                        KSLOG_DEBUG("%s: Unknown ivar type [%s]", ivar->name, ivar->type);
                }
            }
        }
    }
    writer->endContainer(writer);
}

static bool isRestrictedClass(const char* name)
{
    if(g_introspectionRules->restrictedClasses != NULL)
    {
        for(int i = 0; i < g_introspectionRules->restrictedClassesCount; i++)
        {
            if(strcmp(name, g_introspectionRules->restrictedClasses[i]) == 0)
            {
                return true;
            }
        }
    }
    return false;
}

/** Write the contents of a memory location.
 * Also writes meta information about the data.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param address The memory address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
static void writeMemoryContents(const KSCrashReportWriter* const writer,
                                const char* const key,
                                const uintptr_t address,
                                int* limit)
{
    (*limit)--;
    const void* object = (const void*)address;
    writer->beginObject(writer, key);
    {
        writer->addUIntegerElement(writer, KSCrashField_Address, address);
        const char* zombieClassName = kszombie_className(object);
        if(zombieClassName != NULL)
        {
            writer->addStringElement(writer, KSCrashField_LastDeallocObject, zombieClassName);
        }
        switch(ksobjc_objectType(object))
        {
            case KSObjCTypeUnknown:
                if(object == NULL)
                {
                    writer->addStringElement(writer, KSCrashField_Type, KSCrashMemType_NullPointer);
                }
                else if(isValidString(object))
                {
                    writer->addStringElement(writer, KSCrashField_Type, KSCrashMemType_String);
                    writer->addStringElement(writer, KSCrashField_Value, (const char*)object);
                }
                else
                {
                    writer->addStringElement(writer, KSCrashField_Type, KSCrashMemType_Unknown);
                }
                break;
            case KSObjCTypeClass:
                writer->addStringElement(writer, KSCrashField_Type, KSCrashMemType_Class);
                writer->addStringElement(writer, KSCrashField_Class, ksobjc_className(object));
                break;
            case KSObjCTypeObject:
            {
                writer->addStringElement(writer, KSCrashField_Type, KSCrashMemType_Object);
                const char* className = ksobjc_objectClassName(object);
                writer->addStringElement(writer, KSCrashField_Class, className);
                if(!isRestrictedClass(className))
                {
                    switch(ksobjc_objectClassType(object))
                    {
                        case KSObjCClassTypeString:
                            writeNSStringContents(writer, KSCrashField_Value, address, limit);
                            break;
                        case KSObjCClassTypeURL:
                            writeURLContents(writer, KSCrashField_Value, address, limit);
                            break;
                        case KSObjCClassTypeDate:
                            writeDateContents(writer, KSCrashField_Value, address, limit);
                            break;
                        case KSObjCClassTypeArray:
                            if(*limit > 0)
                            {
                                writeArrayContents(writer, KSCrashField_FirstObject, address, limit);
                            }
                            break;
                        case KSObjCClassTypeNumber:
                            writeNumberContents(writer, KSCrashField_Value, address, limit);
                            break;
                        case KSObjCClassTypeDictionary:
                        case KSObjCClassTypeException:
                            // TODO: Implement these.
                            if(*limit > 0)
                            {
                                writeUnknownObjectContents(writer, KSCrashField_Ivars, address, limit);
                            }
                            break;
                        case KSObjCClassTypeUnknown:
                            if(*limit > 0)
                            {
                                writeUnknownObjectContents(writer, KSCrashField_Ivars, address, limit);
                            }
                            break;
                    }
                }
                break;
            }
            case KSObjCTypeBlock:
                writer->addStringElement(writer, KSCrashField_Type, KSCrashMemType_Block);
                const char* className = ksobjc_objectClassName(object);
                writer->addStringElement(writer, KSCrashField_Class, className);
                break;
        }
    }
    writer->endContainer(writer);
}

static bool isValidPointer(const uintptr_t address)
{
    if(address == (uintptr_t)NULL)
    {
        return false;
    }

    if(ksobjc_isTaggedPointer((const void*)address))
    {
        if(!ksobjc_isValidTaggedPointer((const void*)address))
        {
            return false;
        }
    }
    
    return true;
}

/** Write the contents of a memory location only if it contains notable data.
 * Also writes meta information about the data.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param address The memory address.
 */
static void writeMemoryContentsIfNotable(const KSCrashReportWriter* const writer,
                                         const char* const key,
                                         const uintptr_t address)
{
    if(!isValidPointer(address))
    {
        return;
    }

    const void* object = (const void*)address;
    
    if(ksobjc_objectType(object) == KSObjCTypeUnknown &&
       kszombie_className(object) == NULL &&
       !isValidString(object))
    {
        // Nothing notable about this memory location.
        return;
    }

    int limit = kDefaultMemorySearchDepth;
    writeMemoryContents(writer, key, address, &limit);
}

/** Look for a hex value in a string and try to write whatever it references.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param string The string to search.
 */
static void writeAddressReferencedByString(const KSCrashReportWriter* const writer,
                                           const char* const key,
                                           const char* string)
{
    uint64_t address = 0;
    if(string == NULL || !ksstring_extractHexValue(string, (int)strlen(string), &address))
    {
        return;
    }
    
    int limit = kDefaultMemorySearchDepth;
    writeMemoryContents(writer, key, (uintptr_t)address, &limit);
}

#pragma mark Backtrace

/** Write a backtrace entry to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param address The memory address.
 *
 * @param info Information about the nearest symbols to the address.
 */
static void writeBacktraceEntry(const KSCrashReportWriter* const writer,
                                const char* const key,
                                const uintptr_t address,
                                const Dl_info* const info)
{
    writer->beginObject(writer, key);
    {
        if(info->dli_fname != NULL)
        {
            writer->addStringElement(writer, KSCrashField_ObjectName, ksfu_lastPathEntry(info->dli_fname));
        }
        writer->addUIntegerElement(writer, KSCrashField_ObjectAddr, (uintptr_t)info->dli_fbase);
        if(info->dli_sname != NULL)
        {
            const char* sname = info->dli_sname;
            writer->addStringElement(writer, KSCrashField_SymbolName, sname);
        }
        writer->addUIntegerElement(writer, KSCrashField_SymbolAddr, (uintptr_t)info->dli_saddr);
        writer->addUIntegerElement(writer, KSCrashField_InstructionAddr, address);
    }
    writer->endContainer(writer);
}

/** Write a backtrace to the report.
 *
 * @param writer The writer to write the backtrace to.
 *
 * @param key The object key, if needed.
 *
 * @param backtrace The backtrace to write.
 *
 * @param backtraceLength Length of the backtrace.
 *
 * @param skippedEntries The number of entries that were skipped before the
 *                       beginning of backtrace.
 */
static void writeBacktrace(const KSCrashReportWriter* const writer,
                           const char* const key,
                           const uintptr_t* const backtrace,
                           const int backtraceLength,
                           const int skippedEntries)
{
    writer->beginObject(writer, key);
    {
        writer->beginArray(writer, KSCrashField_Contents);
        {
            if(backtraceLength > 0)
            {
                Dl_info symbolicated[backtraceLength];
                ksbt_symbolicate(backtrace, symbolicated, backtraceLength, skippedEntries);

                for(int i = 0; i < backtraceLength; i++)
                {
                    writeBacktraceEntry(writer, NULL, backtrace[i], &symbolicated[i]);
                }
            }
        }
        writer->endContainer(writer);
        writer->addIntegerElement(writer, KSCrashField_Skipped, skippedEntries);
    }
    writer->endContainer(writer);
}

#pragma mark Stack

/** Write a dump of the stack contents to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param machineContext The context to retrieve the stack from.
 *
 * @param isStackOverflow If true, the stack has overflowed.
 */
static void writeStackContents(const KSCrashReportWriter* const writer,
                               const char* const key,
                               const KSMachineContext machineContext,
                               const bool isStackOverflow)
{
    uintptr_t sp = kscpu_stackPointer(machineContext);
    if((void*)sp == NULL)
    {
        return;
    }

    uintptr_t lowAddress = sp + (uintptr_t)(kStackContentsPushedDistance * (int)sizeof(sp) * kscpu_stackGrowDirection() * -1);
    uintptr_t highAddress = sp + (uintptr_t)(kStackContentsPoppedDistance * (int)sizeof(sp) * kscpu_stackGrowDirection());
    if(highAddress < lowAddress)
    {
        uintptr_t tmp = lowAddress;
        lowAddress = highAddress;
        highAddress = tmp;
    }
    writer->beginObject(writer, key);
    {
        writer->addStringElement(writer, KSCrashField_GrowDirection, kscpu_stackGrowDirection() > 0 ? "+" : "-");
        writer->addUIntegerElement(writer, KSCrashField_DumpStart, lowAddress);
        writer->addUIntegerElement(writer, KSCrashField_DumpEnd, highAddress);
        writer->addUIntegerElement(writer, KSCrashField_StackPtr, sp);
        writer->addBooleanElement(writer, KSCrashField_Overflow, isStackOverflow);
        uint8_t stackBuffer[kStackContentsTotalDistance * sizeof(sp)];
        int copyLength = (int)(highAddress - lowAddress);
        if(ksmem_copySafely((void*)lowAddress, stackBuffer, copyLength))
        {
            writer->addDataElement(writer, KSCrashField_Contents, (void*)stackBuffer, copyLength);
        }
        else
        {
            writer->addStringElement(writer, KSCrashField_Error, "Stack contents not accessible");
        }
    }
    writer->endContainer(writer);
}

/** Write any notable addresses near the stack pointer (above and below).
 *
 * @param writer The writer.
 *
 * @param machineContext The context to retrieve the stack from.
 *
 * @param backDistance The distance towards the beginning of the stack to check.
 *
 * @param forwardDistance The distance past the end of the stack to check.
 */
static void writeNotableStackContents(const KSCrashReportWriter* const writer,
                                      const KSMachineContext machineContext,
                                      const int backDistance,
                                      const int forwardDistance)
{
    uintptr_t sp = kscpu_stackPointer(machineContext);
    if((void*)sp == NULL)
    {
        return;
    }

    uintptr_t lowAddress = sp + (uintptr_t)(backDistance * (int)sizeof(sp) * kscpu_stackGrowDirection() * -1);
    uintptr_t highAddress = sp + (uintptr_t)(forwardDistance * (int)sizeof(sp) * kscpu_stackGrowDirection());
    if(highAddress < lowAddress)
    {
        uintptr_t tmp = lowAddress;
        lowAddress = highAddress;
        highAddress = tmp;
    }
    uintptr_t contentsAsPointer;
    char nameBuffer[40];
    for(uintptr_t address = lowAddress; address < highAddress; address += sizeof(address))
    {
        if(ksmem_copySafely((void*)address, &contentsAsPointer, sizeof(contentsAsPointer)))
        {
            sprintf(nameBuffer, "stack@%p", (void*)address);
            writeMemoryContentsIfNotable(writer, nameBuffer, contentsAsPointer);
        }
    }
}


#pragma mark Registers

/** Write the contents of all regular registers to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param machineContext The context to retrieve the registers from.
 */
static void writeBasicRegisters(const KSCrashReportWriter* const writer,
                                const char* const key,
                                const KSMachineContext machineContext)
{
    char registerNameBuff[30];
    const char* registerName;
    writer->beginObject(writer, key);
    {
        const int numRegisters = kscpu_numRegisters();
        for(int reg = 0; reg < numRegisters; reg++)
        {
            registerName = kscpu_registerName(reg);
            if(registerName == NULL)
            {
                snprintf(registerNameBuff, sizeof(registerNameBuff), "r%d", reg);
                registerName = registerNameBuff;
            }
            writer->addUIntegerElement(writer, registerName,
                                       kscpu_registerValue(machineContext, reg));
        }
    }
    writer->endContainer(writer);
}

/** Write the contents of all exception registers to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param machineContext The context to retrieve the registers from.
 */
static void writeExceptionRegisters(const KSCrashReportWriter* const writer,
                                    const char* const key,
                                    const KSMachineContext machineContext)
{
    char registerNameBuff[30];
    const char* registerName;
    writer->beginObject(writer, key);
    {
        const int numRegisters = kscpu_numExceptionRegisters();
        for(int reg = 0; reg < numRegisters; reg++)
        {
            registerName = kscpu_exceptionRegisterName(reg);
            if(registerName == NULL)
            {
                snprintf(registerNameBuff, sizeof(registerNameBuff), "r%d", reg);
                registerName = registerNameBuff;
            }
            writer->addUIntegerElement(writer,registerName,
                                       kscpu_exceptionRegisterValue(machineContext, reg));
        }
    }
    writer->endContainer(writer);
}

/** Write all applicable registers.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param machineContext The context to retrieve the registers from.
 */
static void writeRegisters(const KSCrashReportWriter* const writer,
                           const char* const key,
                           const KSMachineContext machineContext)
{
    writer->beginObject(writer, key);
    {
        writeBasicRegisters(writer, KSCrashField_Basic, machineContext);
        if(ksmc_hasValidExceptionRegisters(machineContext))
        {
            writeExceptionRegisters(writer, KSCrashField_Exception, machineContext);
        }
    }
    writer->endContainer(writer);
}

/** Write any notable addresses contained in the CPU registers.
 *
 * @param writer The writer.
 *
 * @param machineContext The context to retrieve the registers from.
 */
static void writeNotableRegisters(const KSCrashReportWriter* const writer,
                                  const KSMachineContext machineContext)
{
    char registerNameBuff[30];
    const char* registerName;
    const int numRegisters = kscpu_numRegisters();
    for(int reg = 0; reg < numRegisters; reg++)
    {
        registerName = kscpu_registerName(reg);
        if(registerName == NULL)
        {
            snprintf(registerNameBuff, sizeof(registerNameBuff), "r%d", reg);
            registerName = registerNameBuff;
        }
        writeMemoryContentsIfNotable(writer,
                                     registerName,
                                     (uintptr_t)kscpu_registerValue(machineContext, reg));
    }
}

#pragma mark Thread-specific

/** Write any notable addresses in the stack or registers to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param machineContext The context to retrieve the registers from.
 */
static void writeNotableAddresses(const KSCrashReportWriter* const writer,
                                  const char* const key,
                                  const KSMachineContext machineContext)
{
    writer->beginObject(writer, key);
    {
        writeNotableRegisters(writer, machineContext);
        writeNotableStackContents(writer,
                                  machineContext,
                                  kStackNotableSearchBackDistance,
                                  kStackNotableSearchForwardDistance);
    }
    writer->endContainer(writer);
}

/** Write information about a thread to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param crash The crash handler context.
 *
 * @param machineContext The context whose thread to write about.
 *
 * @param shouldWriteNotableAddresses If true, write any notable addresses found.
 *
 * @param searchThreadNames If true, search thread names as well.
 *
 * @param searchQueueNames If true, search queue names as well.
 */
static void writeThread(const KSCrashReportWriter* const writer,
                        const char* const key,
                        const KSCrash_MonitorContext* const crash,
                        const KSMachineContext machineContext,
                        const int threadIndex,
                        const bool shouldWriteNotableAddresses,
                        const bool searchThreadNames,
                        const bool searchQueueNames)
{
    bool isCrashedThread = ksmc_isCrashedContext(machineContext);
    KSLOG_DEBUG("Writing thread %d. is crashed: %d", threadIndex, isCrashedThread);
    char nameBuffer[128];
    uintptr_t backtraceBuffer[kMaxBacktraceDepth];
    int backtraceLength = sizeof(backtraceBuffer) / sizeof(*backtraceBuffer);
    int skippedEntries = 0;
    KSThread thread = ksmc_getThreadFromContext(machineContext);

    uintptr_t* backtrace = getBacktrace(crash,
                                        machineContext,
                                        backtraceBuffer,
                                        &backtraceLength,
                                        &skippedEntries);

    writer->beginObject(writer, key);
    {
        if(backtrace != NULL)
        {
            writeBacktrace(writer, KSCrashField_Backtrace, backtrace, backtraceLength, skippedEntries);
        }
        if(ksmc_canHaveCPUState(machineContext))
        {
            writeRegisters(writer, KSCrashField_Registers, machineContext);
        }
        writer->addIntegerElement(writer, KSCrashField_Index, threadIndex);
        if(searchThreadNames)
        {
            if(ksthread_getThreadName(thread, nameBuffer, sizeof(nameBuffer)) && nameBuffer[0] != 0)
            {
                writer->addStringElement(writer, KSCrashField_Name, nameBuffer);
            }
        }
        if(searchQueueNames)
        {
            if(ksthread_getQueueName(thread, nameBuffer, sizeof(nameBuffer)) && nameBuffer[0] != 0)
            {
                writer->addStringElement(writer, KSCrashField_DispatchQueue, nameBuffer);
            }
        }
        writer->addBooleanElement(writer, KSCrashField_Crashed, isCrashedThread);
        writer->addBooleanElement(writer, KSCrashField_CurrentThread, thread == ksthread_self());
        if(isCrashedThread)
        {
            writeStackContents(writer, KSCrashField_Stack, machineContext, skippedEntries > 0);
            if(shouldWriteNotableAddresses)
            {
                writeNotableAddresses(writer, KSCrashField_NotableAddresses, machineContext);
            }
        }
    }
    writer->endContainer(writer);
}

/** Write information about all threads to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param crash The crash handler context.
 */
static void writeAllThreads(const KSCrashReportWriter* const writer,
                            const char* const key,
                            const KSCrash_MonitorContext* const crash,
                            bool writeNotableAddresses,
                            bool searchThreadNames,
                            bool searchQueueNames)
{
    KSMachineContext context = crash->offendingMachineContext;
    KSThread offendingThread = ksmc_getThreadFromContext(context);
    int threadCount = ksmc_getThreadCount(context);
    KSMC_NEW_CONTEXT(machineContext);

    // Fetch info for all threads.
    writer->beginArray(writer, key);
    {
        KSLOG_DEBUG("Writing %d threads.", threadCount);
        for(int i = 0; i < threadCount; i++)
        {
            KSThread thread = ksmc_getThreadAtIndex(context, i);
            if(thread == offendingThread)
            {
                writeThread(writer, NULL, crash, context, i, writeNotableAddresses, searchThreadNames, searchQueueNames);
            }
            else
            {
                ksmc_getContextForThread(thread, machineContext, false);
                writeThread(writer, NULL, crash, machineContext, i, writeNotableAddresses, searchThreadNames, searchQueueNames);
            }
        }
    }
    writer->endContainer(writer);
}

#pragma mark Global Report Data

/** Write information about a binary image to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param index Which image to write about.
 */
static void writeBinaryImage(const KSCrashReportWriter* const writer,
                             const char* const key,
                             const uint32_t index)
{
    const struct mach_header* header = _dyld_get_image_header(index);
    if(header == NULL)
    {
        return;
    }

    uintptr_t cmdPtr = ksdl_firstCmdAfterHeader(header);
    if(cmdPtr == 0)
    {
        return;
    }

    // Look for the TEXT segment to get the image size.
    // Also look for a UUID command.
    uint64_t imageSize = 0;
    uint64_t imageVmAddr = 0;
    uint8_t* uuid = NULL;

    for(uint32_t iCmd = 0; iCmd < header->ncmds; iCmd++)
    {
        struct load_command* loadCmd = (struct load_command*)cmdPtr;
        switch(loadCmd->cmd)
        {
            case LC_SEGMENT:
            {
                struct segment_command* segCmd = (struct segment_command*)cmdPtr;
                if(strcmp(segCmd->segname, SEG_TEXT) == 0)
                {
                    imageSize = segCmd->vmsize;
                    imageVmAddr = segCmd->vmaddr;
                }
                break;
            }
            case LC_SEGMENT_64:
            {
                struct segment_command_64* segCmd = (struct segment_command_64*)cmdPtr;
                if(strcmp(segCmd->segname, SEG_TEXT) == 0)
                {
                    imageSize = segCmd->vmsize;
                    imageVmAddr = segCmd->vmaddr;
                }
                break;
            }
            case LC_UUID:
            {
                struct uuid_command* uuidCmd = (struct uuid_command*)cmdPtr;
                uuid = uuidCmd->uuid;
                break;
            }
        }
        cmdPtr += loadCmd->cmdsize;
    }

    writer->beginObject(writer, key);
    {
        writer->addUIntegerElement(writer, KSCrashField_ImageAddress, (uintptr_t)header);
        writer->addUIntegerElement(writer, KSCrashField_ImageVmAddress, imageVmAddr);
        writer->addUIntegerElement(writer, KSCrashField_ImageSize, imageSize);
        writer->addStringElement(writer, KSCrashField_Name, _dyld_get_image_name(index));
        writer->addUUIDElement(writer, KSCrashField_UUID, uuid);
        writer->addIntegerElement(writer, KSCrashField_CPUType, header->cputype);
        writer->addIntegerElement(writer, KSCrashField_CPUSubType, header->cpusubtype);
    }
    writer->endContainer(writer);
}

/** Write information about all images to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 */
static void writeBinaryImages(const KSCrashReportWriter* const writer, const char* const key)
{
    const uint32_t imageCount = _dyld_image_count();

    writer->beginArray(writer, key);
    {
        for(uint32_t iImg = 0; iImg < imageCount; iImg++)
        {
            writeBinaryImage(writer, NULL, iImg);
        }
    }
    writer->endContainer(writer);
}

/** Write information about system memory to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 */
static void writeMemoryInfo(const KSCrashReportWriter* const writer, const char* const key)
{
    writer->beginObject(writer, key);
    {
        writer->addUIntegerElement(writer, KSCrashField_Usable, ksmem_usableMemory());
        writer->addUIntegerElement(writer, KSCrashField_Free, ksmem_freeMemory());
    }
    writer->endContainer(writer);
}

/** Write information about the error leading to the crash to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param crash The crash handler context.
 */
static void writeError(const KSCrashReportWriter* const writer,
                       const char* const key,
                       const KSCrash_MonitorContext* const crash)
{
    int machExceptionType = 0;
    kern_return_t machCode = 0;
    kern_return_t machSubCode = 0;
    int sigNum = 0;
    int sigCode = 0;
    const char* exceptionName = NULL;
    const char* crashReason = NULL;

    // Gather common info.
    switch(crash->crashType)
    {
        case KSCrashMonitorTypeMainThreadDeadlock:
            break;
        case KSCrashMonitorTypeMachException:
            machExceptionType = crash->mach.type;
            machCode = (kern_return_t)crash->mach.code;
            if(machCode == KERN_PROTECTION_FAILURE && crash->isStackOverflow)
            {
                // A stack overflow should return KERN_INVALID_ADDRESS, but
                // when a stack blasts through the guard pages at the top of the stack,
                // it generates KERN_PROTECTION_FAILURE. Correct for this.
                machCode = KERN_INVALID_ADDRESS;
            }
            machSubCode = (kern_return_t)crash->mach.subcode;

            sigNum = kssignal_signalForMachException(machExceptionType, machCode);
            break;
        case KSCrashMonitorTypeCPPException:
            machExceptionType = EXC_CRASH;
            sigNum = SIGABRT;
            crashReason = crash->crashReason;
            exceptionName = crash->CPPException.name;
            break;
        case KSCrashMonitorTypeNSException:
            machExceptionType = EXC_CRASH;
            sigNum = SIGABRT;
            exceptionName = crash->NSException.name;
            crashReason = crash->crashReason;
            break;
        case KSCrashMonitorTypeSignal:
            sigNum = crash->signal.signalInfo->si_signo;
            sigCode = crash->signal.signalInfo->si_code;
            machExceptionType = kssignal_machExceptionForSignal(sigNum);
            break;
        case KSCrashMonitorTypeUserReported:
            machExceptionType = EXC_CRASH;
            sigNum = SIGABRT;
            crashReason = crash->crashReason;
            break;
    }

    const char* machExceptionName = ksrc_exceptionName(machExceptionType);
    const char* machCodeName = machCode == 0 ? NULL : ksmemory_kernelReturnCodeName(machCode);
    const char* sigName = kssignal_signalName(sigNum);
    const char* sigCodeName = kssignal_signalCodeName(sigNum, sigCode);

    writer->beginObject(writer, key);
    {
        writer->beginObject(writer, KSCrashField_Mach);
        {
            writer->addUIntegerElement(writer, KSCrashField_Exception, (unsigned)machExceptionType);
            if(machExceptionName != NULL)
            {
                writer->addStringElement(writer, KSCrashField_ExceptionName, machExceptionName);
            }
            writer->addUIntegerElement(writer, KSCrashField_Code, (unsigned)machCode);
            if(machCodeName != NULL)
            {
                writer->addStringElement(writer, KSCrashField_CodeName, machCodeName);
            }
            writer->addUIntegerElement(writer, KSCrashField_Subcode, (unsigned)machSubCode);
        }
        writer->endContainer(writer);

        writer->beginObject(writer, KSCrashField_Signal);
        {
            writer->addUIntegerElement(writer, KSCrashField_Signal, (unsigned)sigNum);
            if(sigName != NULL)
            {
                writer->addStringElement(writer, KSCrashField_Name, sigName);
            }
            writer->addUIntegerElement(writer, KSCrashField_Code, (unsigned)sigCode);
            if(sigCodeName != NULL)
            {
                writer->addStringElement(writer, KSCrashField_CodeName, sigCodeName);
            }
        }
        writer->endContainer(writer);

        writer->addUIntegerElement(writer, KSCrashField_Address, crash->faultAddress);
        if(crashReason != NULL)
        {
            writer->addStringElement(writer, KSCrashField_Reason, crashReason);
        }

        // Gather specific info.
        switch(crash->crashType)
        {
            case KSCrashMonitorTypeMainThreadDeadlock:
                writer->addStringElement(writer, KSCrashField_Type, KSCrashExcType_Deadlock);
                break;
                
            case KSCrashMonitorTypeMachException:
                writer->addStringElement(writer, KSCrashField_Type, KSCrashExcType_Mach);
                break;

            case KSCrashMonitorTypeCPPException:
            {
                writer->addStringElement(writer, KSCrashField_Type, KSCrashExcType_CPPException);
                writer->beginObject(writer, KSCrashField_CPPException);
                {
                    writer->addStringElement(writer, KSCrashField_Name, exceptionName);
                }
                writer->endContainer(writer);
                break;
            }
            case KSCrashMonitorTypeNSException:
            {
                writer->addStringElement(writer, KSCrashField_Type, KSCrashExcType_NSException);
                writer->beginObject(writer, KSCrashField_NSException);
                {
                    writer->addStringElement(writer, KSCrashField_Name, exceptionName);
                    writeAddressReferencedByString(writer, KSCrashField_ReferencedObject, crashReason);
                }
                writer->endContainer(writer);
                break;
            }
            case KSCrashMonitorTypeSignal:
                writer->addStringElement(writer, KSCrashField_Type, KSCrashExcType_Signal);
                break;

            case KSCrashMonitorTypeUserReported:
            {
                writer->addStringElement(writer, KSCrashField_Type, KSCrashExcType_User);
                writer->beginObject(writer, KSCrashField_UserReported);
                {
                    writer->addStringElement(writer, KSCrashField_Name, crash->userException.name);
                    if(crash->userException.language != NULL)
                    {
                        writer->addStringElement(writer, KSCrashField_Language, crash->userException.language);
                    }
                    if(crash->userException.lineOfCode != NULL)
                    {
                        writer->addStringElement(writer, KSCrashField_LineOfCode, crash->userException.lineOfCode);
                    }
                    if(crash->userException.customStackTrace != NULL)
                    {
                        writer->addJSONElement(writer, KSCrashField_Backtrace, crash->userException.customStackTrace, true);
                    }
                }
                writer->endContainer(writer);
                break;
            }
        }
    }
    writer->endContainer(writer);
}

/** Write information about app runtime, etc to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param state The persistent crash handler state.
 */
static void writeAppStats(const KSCrashReportWriter* const writer, const char* const key, KSCrash_State* state)
{
    writer->beginObject(writer, key);
    {
        writer->addBooleanElement(writer, KSCrashField_AppActive, state->applicationIsActive);
        writer->addBooleanElement(writer, KSCrashField_AppInFG, state->applicationIsInForeground);

        writer->addIntegerElement(writer, KSCrashField_LaunchesSinceCrash, state->launchesSinceLastCrash);
        writer->addIntegerElement(writer, KSCrashField_SessionsSinceCrash, state->sessionsSinceLastCrash);
        writer->addFloatingPointElement(writer, KSCrashField_ActiveTimeSinceCrash, state->activeDurationSinceLastCrash);
        writer->addFloatingPointElement(writer, KSCrashField_BGTimeSinceCrash, state->backgroundDurationSinceLastCrash);

        writer->addIntegerElement(writer, KSCrashField_SessionsSinceLaunch, state->sessionsSinceLaunch);
        writer->addFloatingPointElement(writer, KSCrashField_ActiveTimeSinceLaunch, state->activeDurationSinceLaunch);
        writer->addFloatingPointElement(writer, KSCrashField_BGTimeSinceLaunch, state->backgroundDurationSinceLaunch);
    }
    writer->endContainer(writer);
}

/** Write information about this process.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 */
static void writeProcessState(const KSCrashReportWriter* const writer,
                               const char* const key)
{
    writer->beginObject(writer, key);
    {
        const void* excAddress = kszombie_lastDeallocedNSExceptionAddress();
        if(excAddress != NULL)
        {
            writer->beginObject(writer, KSCrashField_LastDeallocedNSException);
            {
                writer->addUIntegerElement(writer, KSCrashField_Address, (uintptr_t)excAddress);
                writer->addStringElement(writer, KSCrashField_Name, kszombie_lastDeallocedNSExceptionName());
                writer->addStringElement(writer, KSCrashField_Reason, kszombie_lastDeallocedNSExceptionReason());
                writeAddressReferencedByString(writer, KSCrashField_ReferencedObject, kszombie_lastDeallocedNSExceptionReason());
            }
            writer->endContainer(writer);
        }
    }
    writer->endContainer(writer);
}

/** Write basic report information.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param type The report type.
 *
 * @param reportID The report ID.
 */
static void writeReportInfo(const KSCrashReportWriter* const writer,
                            const char* const key,
                            const char* const type,
                            const char* const reportID,
                            const char* const processName)
{
    writer->beginObject(writer, key);
    {
        writer->addStringElement(writer, KSCrashField_Version, KSCRASH_REPORT_VERSION);
        writer->addStringElement(writer, KSCrashField_ID, reportID);
        writer->addStringElement(writer, KSCrashField_ProcessName, processName);
        writer->addIntegerElement(writer, KSCrashField_Timestamp, time(NULL));
        writer->addStringElement(writer, KSCrashField_Type, type);
    }
    writer->endContainer(writer);
}

static void writeRecrash(const KSCrashReportWriter* const writer,
                         const char* const key,
                         const char* crashReportPath)
{
    writer->addJSONFileElement(writer, key, crashReportPath, true);
}


#pragma mark Setup

/** Prepare a report writer for use.
 *
 * @oaram writer The writer to prepare.
 *
 * @param context JSON writer contextual information.
 */
static void prepareReportWriter(KSCrashReportWriter* const writer, KSJSONEncodeContext* const context)
{
    writer->addBooleanElement = addBooleanElement;
    writer->addFloatingPointElement = addFloatingPointElement;
    writer->addIntegerElement = addIntegerElement;
    writer->addUIntegerElement = addUIntegerElement;
    writer->addStringElement = addStringElement;
    writer->addTextFileElement = addTextFileElement;
    writer->addJSONFileElement = addJSONElementFromFile;
    writer->addDataElement = addDataElement;
    writer->beginDataElement = beginDataElement;
    writer->appendDataElement = appendDataElement;
    writer->endDataElement = endDataElement;
    writer->addUUIDElement = addUUIDElement;
    writer->addJSONElement = addJSONElement;
    writer->beginObject = beginObject;
    writer->beginArray = beginArray;
    writer->endContainer = endContainer;
    writer->context = context;
}

static void callUserCrashHandler(KSCrash_Context* const crashContext, KSCrashReportWriter* writer)
{
    crashContext->config.onCrashNotify(writer);
}


// ============================================================================
#pragma mark - Main API -
// ============================================================================

void kscrashreport_writeRecrashReport(KSCrash_Context* const crashContext, const char* const path)
{
    BufferedWriter bufferedWriter = {{0}};
    static char tempPath[1000];
    strncpy(tempPath, path, sizeof(tempPath) - 10);
    strncpy(tempPath + strlen(tempPath) - 5, ".old", 5);
    KSLOG_INFO("Writing recrash report to %s", path);

    if(rename(path, tempPath) < 0)
    {
        KSLOG_ERROR("Could not rename %s to %s: %s", path, tempPath, strerror(errno));
    }
    if(!openBufferedWriter(&bufferedWriter, path))
    {
        return;
    }

    g_introspectionRules = &crashContext->config.introspectionRules;
    
    KSJSONEncodeContext jsonContext;
    jsonContext.userData = &bufferedWriter;
    KSCrashReportWriter concreteWriter;
    KSCrashReportWriter* writer = &concreteWriter;
    prepareReportWriter(writer, &jsonContext);

    ksjson_beginEncode(getJsonContext(writer), true, addJSONData, &bufferedWriter);

    writer->beginObject(writer, KSCrashField_Report);
    {
        writeRecrash(writer, KSCrashField_RecrashReport, tempPath);
        flushBufferedWriter(&bufferedWriter);
        if(remove(tempPath) < 0)
        {
            KSLOG_ERROR("Could not remove %s: %s", tempPath, strerror(errno));
        }
        writeReportInfo(writer,
                        KSCrashField_Report,
                        KSCrashReportType_Minimal,
                        crashContext->config.crashID,
                        crashContext->config.processName);
        flushBufferedWriter(&bufferedWriter);

        writer->beginObject(writer, KSCrashField_Crash);
        {
            writeError(writer, KSCrashField_Error, &crashContext->crash);
            flushBufferedWriter(&bufferedWriter);
            int threadIndex = ksmc_indexOfThread(crashContext->crash.offendingMachineContext,
                                                 ksmc_getThreadFromContext(crashContext->crash.offendingMachineContext));
            writeThread(writer,
                        KSCrashField_CrashedThread,
                        &crashContext->crash,
                        crashContext->crash.offendingMachineContext,
                        threadIndex,
                        false, false, false);
            flushBufferedWriter(&bufferedWriter);
        }
        writer->endContainer(writer);
    }
    writer->endContainer(writer);

    ksjson_endEncode(getJsonContext(writer));
    closeBufferedWriter(&bufferedWriter);
}

void kscrashreport_writeStandardReport(KSCrash_Context* const crashContext, const char* const path)
{
    KSLOG_INFO("Writing crash report to %s", path);
    BufferedWriter bufferedWriter = {{0}};

    if(!openBufferedWriter(&bufferedWriter, path))
    {
        return;
    }
    
    g_introspectionRules = &crashContext->config.introspectionRules;

    KSJSONEncodeContext jsonContext;
    jsonContext.userData = &bufferedWriter;
    KSCrashReportWriter concreteWriter;
    KSCrashReportWriter* writer = &concreteWriter;
    prepareReportWriter(writer, &jsonContext);

    ksjson_beginEncode(getJsonContext(writer), true, addJSONData, &bufferedWriter);

    writer->beginObject(writer, KSCrashField_Report);
    {
        writeReportInfo(writer,
                        KSCrashField_Report,
                        KSCrashReportType_Standard,
                        crashContext->config.crashID,
                        crashContext->config.processName);
        flushBufferedWriter(&bufferedWriter);

        writeBinaryImages(writer, KSCrashField_BinaryImages);
        flushBufferedWriter(&bufferedWriter);

        writeProcessState(writer, KSCrashField_ProcessState);
        flushBufferedWriter(&bufferedWriter);

        if(crashContext->config.systemInfoJSON != NULL)
        {
            addJSONElement(writer, KSCrashField_System, crashContext->config.systemInfoJSON, false);
            flushBufferedWriter(&bufferedWriter);
        }
        else
        {
            writer->beginObject(writer, KSCrashField_System);
        }
        writeMemoryInfo(writer, KSCrashField_Memory);
        flushBufferedWriter(&bufferedWriter);
        writeAppStats(writer, KSCrashField_AppStats, &crashContext->state);
        flushBufferedWriter(&bufferedWriter);
        writer->endContainer(writer);

        writer->beginObject(writer, KSCrashField_Crash);
        {
            writeError(writer, KSCrashField_Error, &crashContext->crash);
            flushBufferedWriter(&bufferedWriter);
            writeAllThreads(writer,
                            KSCrashField_Threads,
                            &crashContext->crash,
                            crashContext->config.introspectionRules.enabled,
                            crashContext->config.searchThreadNames,
                            crashContext->config.searchQueueNames);
            flushBufferedWriter(&bufferedWriter);
        }
        writer->endContainer(writer);

        if(crashContext->config.userInfoJSON != NULL)
        {
            addJSONElement(writer, KSCrashField_User, crashContext->config.userInfoJSON, false);
            flushBufferedWriter(&bufferedWriter);
        }
        else
        {
            writer->beginObject(writer, KSCrashField_User);
        }
        if(crashContext->config.onCrashNotify != NULL)
        {
            flushBufferedWriter(&bufferedWriter);
            callUserCrashHandler(crashContext, writer);
            flushBufferedWriter(&bufferedWriter);
        }
        writer->endContainer(writer);
    }
    writer->endContainer(writer);
    
    ksjson_endEncode(getJsonContext(writer));
    closeBufferedWriter(&bufferedWriter);
}

void kscrashreport_logCrash(const KSCrash_Context* const crashContext)
{
    const KSCrash_MonitorContext* crash = &crashContext->crash;
    logCrashType(crash);
    logCrashThreadBacktrace(&crashContext->crash);
}
