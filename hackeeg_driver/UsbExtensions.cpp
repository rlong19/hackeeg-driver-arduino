#include "UsbExtensions.h"

#include <stdio.h>

#include "AdsControl.h"
#include "BoardConfig.h"
#include "DriverState.h"
#include "SerialCommand.h"

extern SerialCommand serialCommand;

static void runExt(const char *verb) {
    char line[96];
    size_t used = (size_t) snprintf(line, sizeof(line), "%s", verb);
    for (int i = 0; i < 4 && used + 1 < sizeof(line); i++) {
        char *arg = serialCommand.next();
        if (!arg) {
            break;
        }
        int wrote = snprintf(line + used, sizeof(line) - used, " %s", arg);
        if (wrote < 0) {
            break;
        }
        used += (size_t) wrote;
    }
    adsControlSetSource(HACKEEG_STREAM_USB);
    char text[360];
    int code = adsControlExec(line, text, sizeof(text));
    send_response(code, text);
}

static void extSrb1(unsigned char, unsigned char) { runExt("srb1"); }
static void extSrb2(unsigned char, unsigned char) { runExt("srb2"); }
static void extBias(unsigned char, unsigned char) { runExt("bias"); }
static void extBiasP(unsigned char, unsigned char) { runExt("biasp"); }
static void extBiasN(unsigned char, unsigned char) { runExt("biasn"); }
static void extBiasCh(unsigned char, unsigned char) { runExt("biasch"); }
static void extLoff(unsigned char, unsigned char) { runExt("loff"); }
static void extLoffP(unsigned char, unsigned char) { runExt("loffp"); }
static void extLoffN(unsigned char, unsigned char) { runExt("loffn"); }
static void extLoffCh(unsigned char, unsigned char) { runExt("loffch"); }
static void extLoffCurrent(unsigned char, unsigned char) { runExt("loff_current"); }
static void extLoffFreq(unsigned char, unsigned char) { runExt("loff_freq"); }
static void extTestSignal(unsigned char, unsigned char) { runExt("testsignal"); }
static void extTestAmp(unsigned char, unsigned char) { runExt("testamp"); }
static void extGain(unsigned char, unsigned char) { runExt("gain"); }
static void extPower(unsigned char, unsigned char) { runExt("power"); }
static void extInput(unsigned char, unsigned char) { runExt("input"); }
static void extSampleRate(unsigned char, unsigned char) { runExt("samplerate"); }
static void extChannels(unsigned char, unsigned char) { runExt("channels"); }
static void extDropped(unsigned char, unsigned char) { runExt("dropped"); }
static void extDefaults(unsigned char, unsigned char) { runExt("defaults"); }
static void extRegs(unsigned char, unsigned char) { runExt("regs"); }

void hackeegAddExtensionCommands() {
    serialCommand.addCommand("srb1", extSrb1);
    serialCommand.addCommand("srb2", extSrb2);
    serialCommand.addCommand("bias", extBias);
    serialCommand.addCommand("biasp", extBiasP);
    serialCommand.addCommand("biasn", extBiasN);
    serialCommand.addCommand("biasch", extBiasCh);
    serialCommand.addCommand("loff", extLoff);
    serialCommand.addCommand("loffp", extLoffP);
    serialCommand.addCommand("loffn", extLoffN);
    serialCommand.addCommand("loffch", extLoffCh);
    serialCommand.addCommand("loff_current", extLoffCurrent);
    serialCommand.addCommand("loff_freq", extLoffFreq);
    serialCommand.addCommand("testsignal", extTestSignal);
    serialCommand.addCommand("testamp", extTestAmp);
    serialCommand.addCommand("gain", extGain);
    serialCommand.addCommand("power", extPower);
    serialCommand.addCommand("input", extInput);
    serialCommand.addCommand("samplerate", extSampleRate);
    serialCommand.addCommand("channels", extChannels);
    serialCommand.addCommand("dropped", extDropped);
    serialCommand.addCommand("defaults", extDefaults);
    serialCommand.addCommand("regs", extRegs);
}
