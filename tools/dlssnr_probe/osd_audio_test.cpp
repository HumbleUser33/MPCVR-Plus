// osd_audio_test.exe: which audio input the statistics follow, and the bit depth they show.
//
// The cases are rampageX's, from Source/OsdPlus/tests/audio_selection.cpp, counted here
// rather than asserted: a failed assert in a release build ends in abort(), which can
// stop the whole battery on Windows' error dialog, and run_all.cmd reads a verdict.

#include "../../Source/OsdPlus/AudioSelection.h"
#include "../../Source/OsdPlus/AudioDepth.h"
#include <cstdio>
#include <initializer_list>
#include <vector>

static int g_failures = 0;

static void Check(bool ok, const char* what)
{
	printf("   %-62s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok) {
		g_failures++;
	}
}

int main()
{
	printf("-- the input the statistics follow (-1: none can be told apart)\n");
	Check(SelectOsdAudioInput({}) == -1, "no input");
	Check(SelectOsdAudioInput({ -1 }) == 0, "one decoder without a selector");
	Check(SelectOsdAudioInput({ 0 }) == -1, "one input said inactive is not taken");
	Check(SelectOsdAudioInput({ -1, -1 }) == -1, "two inputs, neither says: none guessed");
	Check(SelectOsdAudioInput({ 1, 1 }) == -1, "two inputs both active: none guessed");
	// Eight connected tracks, switched DTS, DTS, E-AC-3, AAC and back, without the graph
	// being rebuilt: the active one each time, not the first connected.
	for (int active : { 0, 1, 2, 7, 0 }) {
		std::vector<int> states(8, 0);
		states[active] = 1;
		char what[64];
		snprintf(what, sizeof(what), "eight tracks, the active one is %d", active);
		Check(SelectOsdAudioInput(states) == active, what);
	}
	Check(SelectOsdAudioInput({ -1, 1, -1 }) == 1, "one active among inputs that do not say");

	printf("-- the bit depth shown\n");
	Check(OsdAudioDepth(0x2001, 24) == 24, "DTS's declared 24 bits kept");
	Check(OsdAudioDepth(0x2001, 0) == 0, "DTS with none declared: none invented");
	Check(OsdAudioDepth(0xff, 16) == 16, "AAC's reported 16 bits kept");
	Check(OsdAudioDepth(0, 16) == 16, "a depth without a format tag kept");
	Check(OsdAudioDepth(1, 24) == 24, "PCM 24 bits");
	Check(OsdAudioDepth(3, 32) == 32, "float 32 bits");
	Check(OsdAudioDepth(0x2001, 65535) == 0, "an impossible depth refused");

	printf("\n%d failure(s)\n", g_failures);
	return g_failures ? 1 : 0;
}
