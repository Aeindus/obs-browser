/******************************************************************************
 Copyright (C) 2014 by John R. Bradley <jrb@turrettech.com>
 Copyright (C) 2018 by Hugh Bailey ("Jim") <jim@obsproject.com>

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************/

#pragma once

#include <obs-module.h>

#include "cef-headers.hpp"
#include "browser-app.hpp"
#include <atomic>
#include <functional>
#include <string>
#include <mutex>
#include <vector>
#include <optional>

#if CHROME_VERSION_BUILD < 4103
#include <obs.hpp>
#include <unordered_map>
#include <vector>

struct AudioStream {
	OBSSourceAutoRelease source;
	speaker_layout speakers;
	int channels;
	int sample_rate;
};
#endif

#define EXTENSIONS_AUDIO \
	"*.aac;"         \
	"*.flac;"        \
	"*.mp3;"         \
	"*.ogg;"         \
	"*.oga;"         \
	"*.opus;"        \
	"*.wav"
#define EXTENSIONS_VIDEO \
	"*.av1;"         \
	"*.ogv;"         \
	"*.mp4;"         \
	"*.webm"
#define EXTENSIONS_IMAGE \
	"*.bmp;"         \
	"*.gif;"         \
	"*.jpg;"         \
	"*.jpeg;"        \
	"*.png;"         \
	"*.webp"
#define EXTENSIONS_COMPLEX \
	"*.pdf;"           \
	"*.txt;"           \
	"*.htm;"           \
	"*.html"
#define EXTENSIONS_ALL                                             \
	EXTENSIONS_VIDEO ";" EXTENSIONS_AUDIO ";" EXTENSIONS_IMAGE \
			 ";" EXTENSIONS_COMPLEX

struct media_file_data {
	bool is_file;
	bool is_video;
	std::string url;
	std::string resourcepath;
};

enum class ExtensionType {
	Audio = 0,
	Video = 1,
	Image = 2,
	Complex = 3,
	All = 4
};

enum class ControlLevel : int {
	None,
	ReadObs,
	ReadUser,
	Basic,
	Advanced,
	All,
};
inline constexpr ControlLevel DEFAULT_CONTROL_LEVEL = ControlLevel::ReadObs;

extern bool hwaccel;

struct BrowserSource {
	BrowserSource **p_prev_next = nullptr;
	BrowserSource *next = nullptr;

	obs_source_t *source = nullptr;

	bool tex_sharing_avail = false;
	bool create_browser = false;
	std::recursive_mutex lockBrowser;
	CefRefPtr<CefBrowser> cefBrowser;

	std::string css;
	std::string js;
	gs_texture_t *texture = nullptr;
	gs_texture_t *extra_texture = nullptr;
	uint32_t last_cx = 0;
	uint32_t last_cy = 0;
	gs_color_format last_format = GS_UNKNOWN;

	int media_index = 0;
	std::vector<media_file_data> media_files;

#ifdef ENABLE_BROWSER_SHARED_TEXTURE
#ifdef _WIN32
	void *last_handle = INVALID_HANDLE_VALUE;
#elif defined(__APPLE__)
	void *last_handle = nullptr;
#endif
#endif

	int width = 0;
	int height = 0;
	bool fps_custom = false;
	int fps = 0;
	double canvas_fps = 0;
	bool restart = false;
	bool pdf_toolbar = false;
	bool shutdown_on_invisible = false;
	bool first_update = true;
	bool reroute_audio = true;
	std::atomic<bool> destroying = false;
	ControlLevel webpage_control_level = DEFAULT_CONTROL_LEVEL;
#if defined(BROWSER_EXTERNAL_BEGIN_FRAME_ENABLED) && \
	defined(ENABLE_BROWSER_SHARED_TEXTURE)
	bool reset_frame = false;
#endif
	bool is_showing = false;

	inline void DestroyTextures()
	{
		obs_enter_graphics();
		if (extra_texture) {
			gs_texture_destroy(extra_texture);
			extra_texture = nullptr;
			last_cx = 0;
			last_cy = 0;
			last_format = GS_UNKNOWN;
		}
		if (texture) {
			gs_texture_destroy(texture);
			texture = nullptr;
		}
		obs_leave_graphics();
	}

	/* ---------------------------- */

	bool CreateBrowser();
	void DestroyBrowser();
	void ExecuteOnBrowser(BrowserFunc func, bool async = false);

	/* ---------------------------- */

	BrowserSource(obs_data_t *settings, obs_source_t *source);
	~BrowserSource();

	void Destroy();

	void Update(obs_data_t *settings = nullptr);
	void Tick();
	void Render();
#if CHROME_VERSION_BUILD < 4103
	void ClearAudioStreams();
	void EnumAudioStreams(obs_source_enum_proc_t cb, void *param);
	bool AudioMix(uint64_t *ts_out, struct audio_output_data *audio_output,
		      size_t channels, size_t sample_rate);
	std::mutex audio_sources_mutex;
	std::vector<obs_source_t *> audio_sources;
	std::unordered_map<int, AudioStream> audio_streams;
#endif
	void SendMouseClick(const struct obs_mouse_event *event, int32_t type,
			    bool mouse_up, uint32_t click_count);
	void SendMouseMove(const struct obs_mouse_event *event,
			   bool mouse_leave);
	void SendMouseWheel(const struct obs_mouse_event *event, int x_delta,
			    int y_delta);
	void SendFocus(bool focus);
	void SendKeyClick(const struct obs_key_event *event, bool key_up);
	void SetShowing(bool showing);
	void SetActive(bool active);
	void Refresh();

#if defined(BROWSER_EXTERNAL_BEGIN_FRAME_ENABLED) && \
	defined(ENABLE_BROWSER_SHARED_TEXTURE)
	inline void SignalBeginFrame();
#endif

	void SetBrowser(CefRefPtr<CefBrowser> b);
	CefRefPtr<CefBrowser> GetBrowser();

	std::optional<media_file_data> GetMediaData();
	std::string GetUrl();
	std::string GetTitleForUrl();
	bool IsLocal();
};

void DispatchJSEvent(std::string eventName, std::string jsonString,
		     BrowserSource *browser);
