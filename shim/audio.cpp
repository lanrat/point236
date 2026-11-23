// Win32 shim: waveOut (PCM streaming) and MCI sequencer (MIDI via TinySoundFont) on Web Audio
#include "shim.h"
#include <algorithm>
#define TSF_IMPLEMENTATION
#include "../third_party/tsf.h"
#define TML_IMPLEMENTATION
#include "../third_party/tml.h"

// JS: schedule PCM on a stream; returns the wall-clock time (ms, performance.now domain) when it finishes
EM_JS(double, jsa_write, (int stream, const void* data, int bytes, int channels, int rate, int bits, double gain), {
	return PeterAudio.write(stream, data, bytes, channels, rate, bits, gain);
});
EM_JS(void, jsa_reset, (int stream), { PeterAudio.reset(stream); });
EM_JS(double, jsa_now, (), { return performance.now(); });

// ------------------------------------------------------------------ waveOut
struct WaveDev {
	uint32_t magic = 0x57415645;
	int stream;
	WAVEFORMATEX fmt;
	struct Pending { WAVEHDR* hdr; double end; };
	std::vector<Pending> pending;
	double written = 0;	// bytes
	bool paused = false;
	DWORD volume = 0xffffffff;
};
static std::vector<WaveDev*> g_devs;
static int g_nextStream = 1;

static void dev_pump(WaveDev* d)
{
	double now = jsa_now();
	for (size_t i = 0; i < d->pending.size();)
	{
		if (now >= d->pending[i].end)
		{
			WAVEHDR* h = d->pending[i].hdr;
			h->dwFlags = (h->dwFlags & ~WHDR_INQUEUE) | WHDR_DONE;
			d->pending.erase(d->pending.begin() + i);
		}
		else i++;
	}
}

extern "C" {

MMRESULT WINAPI waveOutOpen(LPHWAVEOUT out, UINT dev, LPCWAVEFORMATEX fmt, DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{
	if (fmt->wFormatTag != WAVE_FORMAT_PCM || (fmt->wBitsPerSample != 8 && fmt->wBitsPerSample != 16) || fmt->nChannels < 1 || fmt->nChannels > 2)
		return WAVERR_BADFORMAT;
	if (flags & WAVE_FORMAT_QUERY) return MMSYSERR_NOERROR;
	WaveDev* d = new WaveDev; d->stream = g_nextStream++; d->fmt = *fmt;
	g_devs.push_back(d);
	*out = (HWAVEOUT)d;
	return MMSYSERR_NOERROR;
}
static WaveDev* wdev(HWAVEOUT h) { return checked<WaveDev>(h, 0x57415645); }
MMRESULT WINAPI waveOutClose(HWAVEOUT h)
{
	WaveDev* d = wdev(h); if (!d) return MMSYSERR_INVALHANDLE;
	jsa_reset(d->stream);
	g_devs.erase(std::remove(g_devs.begin(), g_devs.end(), d), g_devs.end());
	d->magic = 0; delete d;
	return MMSYSERR_NOERROR;
}
MMRESULT WINAPI waveOutPrepareHeader(HWAVEOUT h, LPWAVEHDR hdr, UINT n) { hdr->dwFlags |= WHDR_PREPARED; hdr->dwFlags &= ~WHDR_DONE; return MMSYSERR_NOERROR; }
MMRESULT WINAPI waveOutUnprepareHeader(HWAVEOUT h, LPWAVEHDR hdr, UINT n)
{
	if (hdr->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
	hdr->dwFlags &= ~WHDR_PREPARED; return MMSYSERR_NOERROR;
}
MMRESULT WINAPI waveOutWrite(HWAVEOUT h, LPWAVEHDR hdr, UINT n)
{
	WaveDev* d = wdev(h); if (!d) return MMSYSERR_INVALHANDLE;
	if (!(hdr->dwFlags & WHDR_PREPARED)) return WAVERR_UNPREPARED;
	hdr->dwFlags = (hdr->dwFlags & ~WHDR_DONE) | WHDR_INQUEUE;
	int loops = (hdr->dwFlags & WHDR_BEGINLOOP) && hdr->dwLoops > 1 ? (int)hdr->dwLoops : 1;
	double end = 0;
	double gain = (d->volume & 0xffff) / 65535.0;
	for (int l = 0; l < loops; l++)
		end = jsa_write(d->stream, hdr->lpData, (int)hdr->dwBufferLength, d->fmt.nChannels, d->fmt.nSamplesPerSec, d->fmt.wBitsPerSample, gain);
	d->written += hdr->dwBufferLength * loops;
	d->pending.push_back({hdr, end});
	return MMSYSERR_NOERROR;
}
MMRESULT WINAPI waveOutReset(HWAVEOUT h)
{
	WaveDev* d = wdev(h); if (!d) return MMSYSERR_INVALHANDLE;
	jsa_reset(d->stream);
	for (auto& p : d->pending) p.hdr->dwFlags = (p.hdr->dwFlags & ~WHDR_INQUEUE) | WHDR_DONE;
	d->pending.clear();
	d->written = 0;
	return MMSYSERR_NOERROR;
}
MMRESULT WINAPI waveOutPause(HWAVEOUT h) { WaveDev* d = wdev(h); if (!d) return MMSYSERR_INVALHANDLE; d->paused = true; return MMSYSERR_NOERROR; }
MMRESULT WINAPI waveOutRestart(HWAVEOUT h) { WaveDev* d = wdev(h); if (!d) return MMSYSERR_INVALHANDLE; d->paused = false; return MMSYSERR_NOERROR; }
MMRESULT WINAPI waveOutGetPosition(HWAVEOUT h, LPMMTIME t, UINT n)
{
	WaveDev* d = wdev(h); if (!d) return MMSYSERR_INVALHANDLE;
	dev_pump(d);
	double now = jsa_now();
	double queuedBytes = 0;
	for (auto& p : d->pending)
	{
		double rem = (p.end - now) / 1000.0 * d->fmt.nAvgBytesPerSec;
		queuedBytes = std::max(queuedBytes, rem);
	}
	DWORD played = (DWORD)std::max(0.0, d->written - queuedBytes);
	if (t->wType == TIME_SAMPLES) t->u.sample = played / std::max<WORD>(1, d->fmt.nBlockAlign);
	else if (t->wType == TIME_MS) t->u.ms = (DWORD)(played * 1000.0 / std::max<DWORD>(1, d->fmt.nAvgBytesPerSec));
	else { t->wType = TIME_BYTES; t->u.cb = played; }
	return MMSYSERR_NOERROR;
}

// ------------------------------------------------------------------ ACM (compressed audio) - not supported
MMRESULT WINAPI acmFormatSuggest(HACMDRIVER had, LPWAVEFORMATEX src, LPWAVEFORMATEX dst, DWORD cb, DWORD f) { return ACMERR_NOTPOSSIBLE; }
MMRESULT WINAPI acmStreamOpen(LPHACMSTREAM s, HACMDRIVER d, LPWAVEFORMATEX a, LPWAVEFORMATEX b, LPWAVEFILTER f, DWORD_PTR c, DWORD_PTR i, DWORD fl) { return ACMERR_NOTPOSSIBLE; }
MMRESULT WINAPI acmStreamClose(HACMSTREAM s, DWORD f) { return MMSYSERR_NOERROR; }
MMRESULT WINAPI acmStreamConvert(HACMSTREAM s, LPACMSTREAMHEADER h, DWORD f) { return ACMERR_NOTPOSSIBLE; }
MMRESULT WINAPI acmStreamPrepareHeader(HACMSTREAM s, LPACMSTREAMHEADER h, DWORD f) { return ACMERR_NOTPOSSIBLE; }
MMRESULT WINAPI acmStreamUnprepareHeader(HACMSTREAM s, LPACMSTREAMHEADER h, DWORD f) { return MMSYSERR_NOERROR; }
MMRESULT WINAPI acmStreamSize(HACMSTREAM s, DWORD a, LPDWORD b, DWORD f) { return ACMERR_NOTPOSSIBLE; }

// ------------------------------------------------------------------ mixer / joystick - not available
UINT WINAPI mixerGetNumDevs(void) { return 0; }
MMRESULT WINAPI mixerOpen(LPHMIXER m, UINT id, DWORD_PTR cb, DWORD_PTR inst, DWORD f) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI mixerClose(HMIXER m) { return MMSYSERR_NOERROR; }
MMRESULT WINAPI mixerGetDevCapsA(UINT_PTR id, LPMIXERCAPSA c, UINT n) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI mixerGetLineInfoA(HMIXEROBJ m, LPMIXERLINEA l, DWORD f) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI mixerGetLineControlsA(HMIXEROBJ m, LPMIXERLINECONTROLSA l, DWORD f) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI mixerGetControlDetailsA(HMIXEROBJ m, LPMIXERCONTROLDETAILS d, DWORD f) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI mixerSetControlDetails(HMIXEROBJ m, LPMIXERCONTROLDETAILS d, DWORD f) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI joyGetDevCapsA(UINT_PTR id, LPJOYCAPSA c, UINT n) { return JOYERR_UNPLUGGED; }
MMRESULT WINAPI joyGetPos(UINT id, LPJOYINFO j) { return JOYERR_UNPLUGGED; }

} // extern "C"

// ------------------------------------------------------------------ MIDI sequencer
static tsf* g_sf = nullptr;
static bool g_sfTried = false;
struct MidiSong {
	MCIDEVICEID id = 0;
	tml_message* msgs = nullptr;
	tml_message* cur = nullptr;
	double posMs = 0;		// rendered position
	unsigned lengthMs = 1;
	bool playing = false;
	HWND notify = NULL;
	int stream = 0;
	double queuedUntil = 0;	// wall time (ms) up to which audio is scheduled
};
static MidiSong* g_song = nullptr;
static MCIDEVICEID g_nextDev = 1;
static const int MIDI_RATE = 44100;

static bool load_sf()
{
	if (g_sf || g_sfTried) return g_sf != nullptr;
	g_sfTried = true;
	g_sf = tsf_load_filename("/sys/soundfont.sf2");
	if (!g_sf) { SHIM_LOG("[midi] no soundfont available - music disabled"); return false; }
	tsf_set_output(g_sf, TSF_STEREO_INTERLEAVED, MIDI_RATE, -6.0f);
	tsf_set_max_voices(g_sf, 96);
	tsf_channel_set_bank_preset(g_sf, 9, 128, 0);
	return true;
}

static void midi_seek(MidiSong* s, double ms)
{
	tsf_reset(g_sf);
	for (int ch = 0; ch < 16; ch++) { tsf_channel_set_presetnumber(g_sf, ch, 0, ch == 9); tsf_channel_set_pitchrange(g_sf, ch, 2); }
	// replay state-changing events up to the target position without notes
	tml_message* m = s->msgs;
	for (; m && m->time < ms; m = m->next)
	{
		switch (m->type)
		{
		case TML_PROGRAM_CHANGE: tsf_channel_set_presetnumber(g_sf, m->channel, m->program, m->channel == 9); break;
		case TML_PITCH_BEND: tsf_channel_set_pitchwheel(g_sf, m->channel, m->pitch_bend); break;
		case TML_CONTROL_CHANGE: tsf_channel_midi_control(g_sf, m->channel, m->control, m->control_value); break;
		}
	}
	s->cur = m; s->posMs = ms;
}

static void midi_render(MidiSong* s, int frames, std::vector<short>& buf)
{
	buf.assign(frames * 2, 0);
	short* out = buf.data();
	int left = frames;
	while (left > 0)
	{
		int block = std::min(left, 64);
		double blockEnd = s->posMs + block * 1000.0 / MIDI_RATE;
		for (; s->cur && s->cur->time < blockEnd; s->cur = s->cur->next)
		{
			tml_message* m = s->cur;
			switch (m->type)
			{
			case TML_PROGRAM_CHANGE: tsf_channel_set_presetnumber(g_sf, m->channel, m->program, m->channel == 9); break;
			case TML_NOTE_ON: tsf_channel_note_on(g_sf, m->channel, m->key, m->velocity / 127.0f); break;
			case TML_NOTE_OFF: tsf_channel_note_off(g_sf, m->channel, m->key); break;
			case TML_PITCH_BEND: tsf_channel_set_pitchwheel(g_sf, m->channel, m->pitch_bend); break;
			case TML_CONTROL_CHANGE: tsf_channel_midi_control(g_sf, m->channel, m->control, m->control_value); break;
			}
		}
		tsf_render_short(g_sf, out, block, 0);
		out += block * 2; left -= block; s->posMs = blockEnd;
	}
}

void midi_pump()
{
	MidiSong* s = g_song;
	if (!s || !s->playing || !g_sf) return;
	double now = jsa_now();
	if (s->queuedUntil < now) s->queuedUntil = now;
	std::vector<short> buf;
	while (s->queuedUntil - now < 300 && s->playing)
	{
		if (s->posMs >= s->lengthMs)
		{
			s->playing = false;
			if (s->notify) post_msg(s->notify, MM_MCINOTIFY, MCI_NOTIFY_SUCCESSFUL, s->id);
			break;
		}
		int frames = MIDI_RATE / 10;	// 100 ms chunks
		midi_render(s, frames, buf);
		s->queuedUntil = jsa_write(s->stream, buf.data(), frames * 4, 2, MIDI_RATE, 16, 1.0);
	}
}

void audio_pump()
{
	for (auto d : g_devs) dev_pump(d);
	midi_pump();
}

static double song_pos_ms(MidiSong* s)
{
	// position actually heard = rendered position minus what is still queued
	if (!s->playing) return s->posMs;
	double q = std::max(0.0, s->queuedUntil - jsa_now());
	return std::max(0.0, s->posMs - q);
}

extern "C" MCIERROR WINAPI mciSendCommandA(MCIDEVICEID dev, UINT msg, DWORD_PTR flags, DWORD_PTR parm)
{
	switch (msg)
	{
	case MCI_OPEN:
	{
		auto* p = (MCI_OPEN_PARMSA*)parm;
		if (!(flags & MCI_OPEN_TYPE) || !p->lpstrDeviceType || strcasecmp(p->lpstrDeviceType, "sequencer") != 0) return MCIERR_DEVICE_NOT_INSTALLED;
		if (!load_sf()) return MCIERR_DEVICE_NOT_INSTALLED;
		std::string path = win_to_unix(p->lpstrElementName);
		tml_message* msgs = tml_load_filename(path.c_str());
		if (!msgs) return MCIERR_INVALID_FILE;
		if (g_song) { jsa_reset(g_song->stream); tml_free(g_song->msgs); delete g_song; }
		MidiSong* s = new MidiSong; s->msgs = msgs; s->id = g_nextDev++; s->stream = 1000 + s->id;
		unsigned len = 0; tml_get_info(msgs, nullptr, nullptr, nullptr, nullptr, &len);
		s->lengthMs = std::max(1u, len + 500);	// allow release tails
		midi_seek(s, 0);
		g_song = s;
		p->wDeviceID = s->id;
		return 0;
	}
	}
	MidiSong* s = g_song;
	if (!s || s->id != dev) return MCIERR_INVALID_DEVICE_ID;
	switch (msg)
	{
	case MCI_SET: return 0;
	case MCI_STATUS:
	{
		auto* p = (MCI_STATUS_PARMS*)parm;
		if (p->dwItem == MCI_STATUS_LENGTH) p->dwReturn = s->lengthMs;
		else if (p->dwItem == MCI_STATUS_POSITION) p->dwReturn = (DWORD)song_pos_ms(s);
		else if (p->dwItem == MCI_STATUS_MODE) p->dwReturn = s->playing ? MCI_MODE_PLAY : MCI_MODE_PAUSE;
		else p->dwReturn = 0;
		return 0;
	}
	case MCI_SEEK:
	{
		auto* p = (MCI_SEEK_PARMS*)parm;
		bool was = s->playing; s->playing = false; jsa_reset(s->stream);
		midi_seek(s, (flags & MCI_TO) ? (double)p->dwTo : 0.0);
		s->queuedUntil = 0;
		s->playing = false; (void)was;
		return 0;
	}
	case MCI_PLAY:
	{
		auto* p = (MCI_PLAY_PARMS*)parm;
		if (p && (flags & MCI_FROM)) midi_seek(s, p->dwFrom);
		s->notify = (flags & MCI_NOTIFY) && p ? (HWND)p->dwCallback : NULL;
		s->playing = true; s->queuedUntil = 0;
		midi_pump();
		return 0;
	}
	case MCI_PAUSE: case MCI_STOP:
	{
		double pos = song_pos_ms(s);
		s->playing = false; jsa_reset(s->stream);
		tsf_note_off_all(g_sf);
		midi_seek(s, pos);
		return 0;
	}
	case MCI_CLOSE:
		jsa_reset(s->stream); tml_free(s->msgs); delete s; g_song = nullptr;
		if (g_sf) tsf_note_off_all(g_sf);
		return 0;
	}
	return 0;
}
