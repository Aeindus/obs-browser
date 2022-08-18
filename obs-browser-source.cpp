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

#include "obs-browser-source.hpp"
#include "browser-client.hpp"
#include "browser-scheme.hpp"
#include "wide-string.hpp"
#include "json11/json11.hpp"
#include <util/threading.h>
#include <util/platform.h>
#include <QApplication>
#include <util/dstr.h>
#include <algorithm>
#include <functional>
#include <thread>
#include <mutex>
#include <vector>

#ifdef __linux__
#include "linux-keyboard-helpers.hpp"
#endif

#ifdef ENABLE_BROWSER_QT_LOOP
#include <QEventLoop>
#include <QThread>
#endif

using namespace std;
using namespace json11;

static void add_file(std::vector<media_file_data> &list, std::string path,
		     bool from_folder = false);
static bool valid_extension(const char *ext);
static std::string to_lower(std::string str);

extern bool QueueCEFTask(std::function<void()> task);

static mutex browser_list_mutex;
static BrowserSource *first_browser = nullptr;

static void SendBrowserVisibility(CefRefPtr<CefBrowser> browser, bool isVisible)
{
	if (!browser)
		return;

#if ENABLE_WASHIDDEN
	if (isVisible) {
		browser->GetHost()->WasResized();
		browser->GetHost()->WasHidden(false);
		browser->GetHost()->Invalidate(PET_VIEW);
	} else {
		browser->GetHost()->WasHidden(true);
	}
#endif

	CefRefPtr<CefProcessMessage> msg =
		CefProcessMessage::Create("Visibility");
	CefRefPtr<CefListValue> args = msg->GetArgumentList();
	args->SetBool(0, isVisible);
	SendBrowserProcessMessage(browser, PID_RENDERER, msg);
}

void DispatchJSEvent(std::string eventName, std::string jsonString,
		     BrowserSource *browser = nullptr);

BrowserSource::BrowserSource(obs_data_t *, obs_source_t *source_)
	: source(source_)
{

	/* Register Refresh hotkey */
	auto refreshFunction = [](void *data, obs_hotkey_id, obs_hotkey_t *,
				  bool pressed) {
		if (pressed) {
			BrowserSource *bs = (BrowserSource *)data;
			bs->Refresh();
		}
	};

	obs_hotkey_register_source(source, "ObsBrowser.Refresh",
				   obs_module_text("RefreshNoCache"),
				   refreshFunction, (void *)this);

	/* defer update */
	obs_source_update(source, nullptr);

	lock_guard<mutex> lock(browser_list_mutex);
	p_prev_next = &first_browser;
	next = first_browser;
	if (first_browser)
		first_browser->p_prev_next = &next;
	first_browser = this;
}

static void ActuallyCloseBrowser(CefRefPtr<CefBrowser> cefBrowser)
{
	CefRefPtr<CefClient> client = cefBrowser->GetHost()->GetClient();
	BrowserClient *bc = reinterpret_cast<BrowserClient *>(client.get());
	if (bc) {
		bc->bs = nullptr;
	}

	/*
         * This stops rendering
         * http://magpcss.org/ceforum/viewtopic.php?f=6&t=12079
         * https://bitbucket.org/chromiumembedded/cef/issues/1363/washidden-api-got-broken-on-branch-2062)
         */
	cefBrowser->GetHost()->WasHidden(true);
	cefBrowser->GetHost()->CloseBrowser(true);
}

BrowserSource::~BrowserSource()
{
	if (cefBrowser)
		ActuallyCloseBrowser(cefBrowser);
}

void BrowserSource::Destroy()
{
	destroying = true;
	DestroyTextures();

	lock_guard<mutex> lock(browser_list_mutex);
	if (next)
		next->p_prev_next = p_prev_next;
	*p_prev_next = next;

	QueueCEFTask([this]() { delete this; });
}

void BrowserSource::ExecuteOnBrowser(BrowserFunc func, bool async)
{
	if (!async) {
#ifdef ENABLE_BROWSER_QT_LOOP
		if (QThread::currentThread() == qApp->thread()) {
			if (!!cefBrowser)
				func(cefBrowser);
			return;
		}
#endif
		os_event_t *finishedEvent;
		os_event_init(&finishedEvent, OS_EVENT_TYPE_AUTO);
		bool success = QueueCEFTask([&]() {
			if (!!cefBrowser)
				func(cefBrowser);
			os_event_signal(finishedEvent);
		});
		if (success) {
			os_event_wait(finishedEvent);
		}
		os_event_destroy(finishedEvent);
	} else {
		CefRefPtr<CefBrowser> browser = GetBrowser();
		if (!!browser) {
#ifdef ENABLE_BROWSER_QT_LOOP
			QueueBrowserTask(cefBrowser, func);
#else
			QueueCEFTask([=]() { func(browser); });
#endif
		}
	}
}

bool BrowserSource::CreateBrowser()
{
	return QueueCEFTask([this]() {
#ifdef ENABLE_BROWSER_SHARED_TEXTURE
		if (hwaccel) {
			obs_enter_graphics();
			tex_sharing_avail = gs_shared_texture_available();
			obs_leave_graphics();
		}
#else
		bool hwaccel = false;
#endif

		CefRefPtr<BrowserClient> browserClient =
			new BrowserClient(this, hwaccel && tex_sharing_avail,
					  reroute_audio, webpage_control_level);

		CefWindowInfo windowInfo;
#if CHROME_VERSION_BUILD < 4430
		windowInfo.width = width;
		windowInfo.height = height;
#else
		windowInfo.bounds.width = width;
		windowInfo.bounds.height = height;
#endif
		windowInfo.windowless_rendering_enabled = true;

#ifdef ENABLE_BROWSER_SHARED_TEXTURE
		windowInfo.shared_texture_enabled = hwaccel;
#endif

		CefBrowserSettings cefBrowserSettings;

#ifdef ENABLE_BROWSER_SHARED_TEXTURE
#ifdef BROWSER_EXTERNAL_BEGIN_FRAME_ENABLED
		if (!fps_custom) {
			windowInfo.external_begin_frame_enabled = true;
			cefBrowserSettings.windowless_frame_rate = 0;
		} else {
			cefBrowserSettings.windowless_frame_rate = fps;
		}
#else
		struct obs_video_info ovi;
		obs_get_video_info(&ovi);
		canvas_fps = (double)ovi.fps_num / (double)ovi.fps_den;
		cefBrowserSettings.windowless_frame_rate =
			(fps_custom) ? fps : canvas_fps;
#endif
#else
		cefBrowserSettings.windowless_frame_rate = fps;
#endif

		cefBrowserSettings.default_font_size = 16;
		cefBrowserSettings.default_fixed_font_size = 16;

#if ENABLE_LOCAL_FILE_URL_SCHEME && CHROME_VERSION_BUILD < 4430
		if (IsLocal()) {
			/* Disable web security for file:// URLs to allow
			 * local content access to remote APIs */
			cefBrowserSettings.web_security = STATE_DISABLED;
		}
#endif
		auto browser = CefBrowserHost::CreateBrowserSync(
			windowInfo, browserClient, GetUrl(), cefBrowserSettings,
			CefRefPtr<CefDictionaryValue>(), nullptr);

		SetBrowser(browser);

		if (reroute_audio)
			cefBrowser->GetHost()->SetAudioMuted(true);
		if (obs_source_showing(source))
			is_showing = true;

		SendBrowserVisibility(cefBrowser, is_showing);
	});
}

void BrowserSource::DestroyBrowser()
{
	ExecuteOnBrowser(ActuallyCloseBrowser, true);
	SetBrowser(nullptr);
}
#if CHROME_VERSION_BUILD < 4103
void BrowserSource::ClearAudioStreams()
{
	QueueCEFTask([this]() {
		audio_streams.clear();
		std::lock_guard<std::mutex> lock(audio_sources_mutex);
		audio_sources.clear();
	});
}
#endif
void BrowserSource::SendMouseClick(const struct obs_mouse_event *event,
				   int32_t type, bool mouse_up,
				   uint32_t click_count)
{
	uint32_t modifiers = event->modifiers;
	int32_t x = event->x;
	int32_t y = event->y;

	ExecuteOnBrowser(
		[=](CefRefPtr<CefBrowser> cefBrowser) {
			CefMouseEvent e;
			e.modifiers = modifiers;
			e.x = x;
			e.y = y;
			CefBrowserHost::MouseButtonType buttonType =
				(CefBrowserHost::MouseButtonType)type;
			cefBrowser->GetHost()->SendMouseClickEvent(
				e, buttonType, mouse_up, click_count);
		},
		true);
}

void BrowserSource::SendMouseMove(const struct obs_mouse_event *event,
				  bool mouse_leave)
{
	uint32_t modifiers = event->modifiers;
	int32_t x = event->x;
	int32_t y = event->y;

	ExecuteOnBrowser(
		[=](CefRefPtr<CefBrowser> cefBrowser) {
			CefMouseEvent e;
			e.modifiers = modifiers;
			e.x = x;
			e.y = y;
			cefBrowser->GetHost()->SendMouseMoveEvent(e,
								  mouse_leave);
		},
		true);
}

void BrowserSource::SendMouseWheel(const struct obs_mouse_event *event,
				   int x_delta, int y_delta)
{
	uint32_t modifiers = event->modifiers;
	int32_t x = event->x;
	int32_t y = event->y;

	ExecuteOnBrowser(
		[=](CefRefPtr<CefBrowser> cefBrowser) {
			CefMouseEvent e;
			e.modifiers = modifiers;
			e.x = x;
			e.y = y;
			cefBrowser->GetHost()->SendMouseWheelEvent(e, x_delta,
								   y_delta);
		},
		true);
}

void BrowserSource::SendFocus(bool focus)
{
	ExecuteOnBrowser(
		[=](CefRefPtr<CefBrowser> cefBrowser) {
#if CHROME_VERSION_BUILD < 4430
			cefBrowser->GetHost()->SendFocusEvent(focus);
#else
			cefBrowser->GetHost()->SetFocus(focus);
#endif
		},
		true);
}

void BrowserSource::SendKeyClick(const struct obs_key_event *event, bool key_up)
{
	if (destroying)
		return;

	std::string text = event->text;
#ifdef __linux__
	uint32_t native_vkey = KeyboardCodeFromXKeysym(event->native_vkey);
	uint32_t modifiers = event->native_modifiers;
#elif defined(_WIN32) || defined(__APPLE__)
	uint32_t native_vkey = event->native_vkey;
	uint32_t modifiers = event->modifiers;
#else
	uint32_t native_vkey = event->native_vkey;
	uint32_t native_scancode = event->native_scancode;
	uint32_t modifiers = event->native_modifiers;
#endif

	ExecuteOnBrowser(
		[=](CefRefPtr<CefBrowser> cefBrowser) {
			CefKeyEvent e;
			e.windows_key_code = native_vkey;
#ifdef __APPLE__
			e.native_key_code = native_vkey;
#endif

			e.type = key_up ? KEYEVENT_KEYUP : KEYEVENT_RAWKEYDOWN;

			if (!text.empty()) {
				wstring wide = to_wide(text);
				if (wide.size())
					e.character = wide[0];
			}

			//e.native_key_code = native_vkey;
			e.modifiers = modifiers;

			cefBrowser->GetHost()->SendKeyEvent(e);
			if (!text.empty() && !key_up) {
				e.type = KEYEVENT_CHAR;
#ifdef __linux__
				e.windows_key_code =
					KeyboardCodeFromXKeysym(e.character);
#elif defined(_WIN32)
				e.windows_key_code = e.character;
#elif !defined(__APPLE__)
				e.native_key_code = native_scancode;
#endif
				cefBrowser->GetHost()->SendKeyEvent(e);
			}
		},
		true);
}

void BrowserSource::SetShowing(bool showing)
{
	if (destroying)
		return;

	is_showing = showing;

	if (shutdown_on_invisible) {
		if (showing) {
			Update();
		} else {
			DestroyBrowser();
		}
	} else {
		ExecuteOnBrowser(
			[=](CefRefPtr<CefBrowser> cefBrowser) {
				CefRefPtr<CefProcessMessage> msg =
					CefProcessMessage::Create("Visibility");
				CefRefPtr<CefListValue> args =
					msg->GetArgumentList();
				args->SetBool(0, showing);
				SendBrowserProcessMessage(cefBrowser,
							  PID_RENDERER, msg);
			},
			true);
		Json json = Json::object{{"visible", showing}};
		DispatchJSEvent("obsSourceVisibleChanged", json.dump(), this);
#if defined(BROWSER_EXTERNAL_BEGIN_FRAME_ENABLED) && \
	defined(ENABLE_BROWSER_SHARED_TEXTURE)
		if (showing && !fps_custom) {
			reset_frame = false;
		}
#endif

		SendBrowserVisibility(cefBrowser, showing);

		if (showing)
			return;

		obs_enter_graphics();

		if (!hwaccel && texture) {
			DestroyTextures();
		}

		obs_leave_graphics();
	}
}

void BrowserSource::SetActive(bool active)
{
	ExecuteOnBrowser(
		[=](CefRefPtr<CefBrowser> cefBrowser) {
			CefRefPtr<CefProcessMessage> msg =
				CefProcessMessage::Create("Active");
			CefRefPtr<CefListValue> args = msg->GetArgumentList();
			args->SetBool(0, active);
			SendBrowserProcessMessage(cefBrowser, PID_RENDERER,
						  msg);
		},
		true);
	Json json = Json::object{{"active", active}};
	DispatchJSEvent("obsSourceActiveChanged", json.dump(), this);
}

void BrowserSource::Refresh()
{
	ExecuteOnBrowser(
		[](CefRefPtr<CefBrowser> cefBrowser) {
			cefBrowser->ReloadIgnoreCache();
		},
		true);
}

void BrowserSource::SetBrowser(CefRefPtr<CefBrowser> b)
{
	std::lock_guard<std::recursive_mutex> auto_lock(lockBrowser);
	cefBrowser = b;
}

CefRefPtr<CefBrowser> BrowserSource::GetBrowser()
{
	std::lock_guard<std::recursive_mutex> auto_lock(lockBrowser);
	return cefBrowser;
}

std::string BrowserSource::GetUrl()
{
	if (media_files.empty())
		return "";
	return media_files[media_index].url;
}

bool BrowserSource::IsLocal()
{
	if (media_files.empty())
		return false;
	return media_files[media_index].is_file;
}

#ifdef ENABLE_BROWSER_SHARED_TEXTURE
#ifdef BROWSER_EXTERNAL_BEGIN_FRAME_ENABLED
inline void BrowserSource::SignalBeginFrame()
{
	if (reset_frame) {
		ExecuteOnBrowser(
			[](CefRefPtr<CefBrowser> cefBrowser) {
				cefBrowser->GetHost()->SendExternalBeginFrame();
			},
			true);

		reset_frame = false;
	}
}
#endif
#endif

void BrowserSource::Update(obs_data_t *settings)
{
	if (settings) {
		obs_data_array_t *playlist_array;
		size_t playlist_count;
		int n_width;
		int n_height;
		bool n_fps_custom;
		int n_fps;
		bool n_shutdown;
		bool n_restart;
		bool n_reroute;
		ControlLevel n_webpage_control_level;
		std::string n_url;
		std::string n_css;
		std::string n_js;
		std::string new_url;
		std::string last_url;

		// -------------------------------------------
		// Add files, folders and urls to list
		last_url = GetUrl();
		media_files.clear();

		playlist_array = obs_data_get_array(settings, "playlist");
		playlist_count = obs_data_array_count(playlist_array);

		for (size_t i = 0; i < playlist_count; i++) {
			obs_data_t *item =
				obs_data_array_item(playlist_array, i);
			string path = obs_data_get_string(item, "value");
			os_dir_t *dir = os_opendir(path.c_str());

			if (dir) {
				os_dirent *ent;
				std::vector<media_file_data> folder_files;

				for (;;) {
					const char *ext;

					ent = os_readdir(dir);
					if (!ent)
						break;
					if (ent->directory)
						continue;

					ext = os_get_path_extension(
						ent->d_name);
					if (!valid_extension(ext))
						continue;

					std::string filepath = path + "/";
					filepath += ent->d_name;
					add_file(folder_files, filepath, true);
				}

				// Sort files from this folder and append to result
				std::sort(
					folder_files.begin(),
					folder_files.end(),
					[](const auto &a, const auto &b) {
						return to_lower(a.filepath)
							       .compare(to_lower(
								       b.filepath)) <=
						       0;
					});
				media_files.insert(media_files.end(),
						   folder_files.begin(),
						   folder_files.end());

				os_closedir(dir);
			} else {
				add_file(media_files, path);
			}

			obs_data_release(item);
		}
		obs_data_array_release(playlist_array);

		// See if the same file can be found again and go to its index
		auto search_result =
			std::find_if(media_files.begin(), media_files.end(),
				     [&last_url](const auto &el) {
					     return el.url == last_url;
				     });
		if (search_result == media_files.end()) {
			media_index = 0; // Reset to beginning
		} else {
			media_index = search_result - media_files.begin();
		}
		// -------------------------------------------

		n_width = (int)obs_data_get_int(settings, "width");
		n_height = (int)obs_data_get_int(settings, "height");
		n_fps_custom = obs_data_get_bool(settings, "fps_custom");
		n_fps = (int)obs_data_get_int(settings, "fps");
		n_shutdown = obs_data_get_bool(settings, "shutdown");
		n_restart = obs_data_get_bool(settings, "restart_when_active");
		n_css = obs_data_get_string(settings, "css");
		n_js = obs_data_get_string(settings, "js");
		n_reroute = obs_data_get_bool(settings, "reroute_audio");
		n_webpage_control_level = static_cast<ControlLevel>(
			obs_data_get_int(settings, "webpage_control_level"));
		new_url = GetUrl();

		// Here add check to see if we just added new files and that
		// no reset is necesarily.
		if (n_fps_custom == fps_custom && n_fps == fps &&
		    n_shutdown == shutdown_on_invisible &&
		    last_url == new_url && n_restart == restart &&
		    n_css == css && n_js == js && n_reroute == reroute_audio &&
		    n_webpage_control_level == webpage_control_level) {

			if (n_width == width && n_height == height)
				return;

			width = n_width;
			height = n_height;
			ExecuteOnBrowser(
				[=](CefRefPtr<CefBrowser> cefBrowser) {
					const CefSize cefSize(width, height);
					cefBrowser->GetHost()
						->GetClient()
						->GetDisplayHandler()
						->OnAutoResize(cefBrowser,
							       cefSize);
					cefBrowser->GetHost()->WasResized();
					cefBrowser->GetHost()->Invalidate(
						PET_VIEW);
				},
				true);
			return;
		}

		width = n_width;
		height = n_height;
		fps = n_fps;
		fps_custom = n_fps_custom;
		shutdown_on_invisible = n_shutdown;
		reroute_audio = n_reroute;
		webpage_control_level = n_webpage_control_level;
		restart = n_restart;
		css = n_css;
		js = n_js;

		obs_source_set_audio_active(source, reroute_audio);
	}

	DestroyBrowser();
	DestroyTextures();
#if CHROME_VERSION_BUILD < 4103
	ClearAudioStreams();
#endif
	if (!shutdown_on_invisible || obs_source_showing(source))
		create_browser = true;

	first_update = false;
}

void BrowserSource::Tick()
{
	if (create_browser && CreateBrowser())
		create_browser = false;
#if defined(ENABLE_BROWSER_SHARED_TEXTURE)
#if defined(BROWSER_EXTERNAL_BEGIN_FRAME_ENABLED)
	if (!fps_custom)
		reset_frame = true;
#else
	struct obs_video_info ovi;
	obs_get_video_info(&ovi);
	double video_fps = (double)ovi.fps_num / (double)ovi.fps_den;

	if (!fps_custom) {
		if (!!cefBrowser && canvas_fps != video_fps) {
			cefBrowser->GetHost()->SetWindowlessFrameRate(
				video_fps);
			canvas_fps = video_fps;
		}
	}
#endif
#endif
}

extern void ProcessCef();

void BrowserSource::Render()
{
	bool flip = false;
#ifdef ENABLE_BROWSER_SHARED_TEXTURE
	flip = hwaccel;
#endif

	if (texture) {
#ifdef __APPLE__
		gs_effect_t *effect =
			obs_get_base_effect((hwaccel) ? OBS_EFFECT_DEFAULT_RECT
						      : OBS_EFFECT_DEFAULT);
#else
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
#endif

		bool linear_sample = extra_texture == NULL;
		gs_texture_t *draw_texture = texture;
		if (!linear_sample &&
		    !obs_source_get_texcoords_centered(source)) {
			gs_copy_texture(extra_texture, texture);
			draw_texture = extra_texture;

			linear_sample = true;
		}

		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

		gs_eparam_t *const image =
			gs_effect_get_param_by_name(effect, "image");

		const char *tech;
		if (linear_sample) {
			gs_effect_set_texture_srgb(image, draw_texture);
			tech = "Draw";
		} else {
			gs_effect_set_texture(image, draw_texture);
			tech = "DrawSrgbDecompress";
		}

		const uint32_t flip_flag = flip ? GS_FLIP_V : 0;
		while (gs_effect_loop(effect, tech))
			gs_draw_sprite(draw_texture, flip_flag, 0, 0);

		gs_blend_state_pop();

		gs_enable_framebuffer_srgb(previous);
	}

#if defined(BROWSER_EXTERNAL_BEGIN_FRAME_ENABLED) && \
	defined(ENABLE_BROWSER_SHARED_TEXTURE)
	SignalBeginFrame();
#elif defined(ENABLE_BROWSER_QT_LOOP)
	ProcessCef();
#endif
}

static void ExecuteOnBrowser(BrowserFunc func, BrowserSource *bs)
{
	lock_guard<mutex> lock(browser_list_mutex);

	if (bs) {
		BrowserSource *bsw = reinterpret_cast<BrowserSource *>(bs);
		bsw->ExecuteOnBrowser(func, true);
	}
}

static void ExecuteOnAllBrowsers(BrowserFunc func)
{
	lock_guard<mutex> lock(browser_list_mutex);

	BrowserSource *bs = first_browser;
	while (bs) {
		BrowserSource *bsw = reinterpret_cast<BrowserSource *>(bs);
		bsw->ExecuteOnBrowser(func, true);
		bs = bs->next;
	}
}

void DispatchJSEvent(std::string eventName, std::string jsonString,
		     BrowserSource *browser)
{
	const auto jsEvent = [=](CefRefPtr<CefBrowser> cefBrowser) {
		CefRefPtr<CefProcessMessage> msg =
			CefProcessMessage::Create("DispatchJSEvent");
		CefRefPtr<CefListValue> args = msg->GetArgumentList();

		args->SetString(0, eventName);
		args->SetString(1, jsonString);
		SendBrowserProcessMessage(cefBrowser, PID_RENDERER, msg);
	};

	if (!browser)
		ExecuteOnAllBrowsers(jsEvent);
	else
		ExecuteOnBrowser(jsEvent, browser);
}

static void add_file(std::vector<media_file_data> &list, std::string path,
		     bool from_folder)
{
	media_file_data data;
	bool is_file = from_folder ? true
				   : (path.find("://") == std::string::npos);

	// Save original value
	data.filepath = path;

	// If path is a file then encode
	if (is_file && !path.empty()) {
		path = CefURIEncode(path, false);

#ifdef _WIN32
		size_t slash = path.find("%2F");
		size_t colon = path.find("%3A");

		if (slash != std::string::npos && colon != std::string::npos &&
		    colon < slash)
			path.replace(colon, 3, ":");
#endif

		while (path.find("%5C") != std::string::npos)
			path.replace(path.find("%5C"), 3, "/");

		while (path.find("%2F") != std::string::npos)
			path.replace(path.find("%2F"), 3, "/");

#if !ENABLE_LOCAL_FILE_URL_SCHEME
		/* http://absolute/ based mapping for older CEF */
		path = "http://absolute/" + path;
#elif defined(_WIN32)
		/* Widows-style local file URL:
			 * file:///C:/file/path.webm */
		path = "file:///" + path;
#else
		/* UNIX-style local file URL:
			 * file:///home/user/file.webm */
		path = "file://" + path;
#endif
	}

#if ENABLE_LOCAL_FILE_URL_SCHEME
	if (astrcmpi_n(path.c_str(), "http://absolute/", 16) == 0) {
		/* Replace http://absolute/ URLs with file://
			 * URLs if file:// URLs are enabled */
		path = "file:///" + path.substr(16);
		is_file = true;
	}
#endif

	data.is_file = is_file;
	data.url = path;

	list.push_back(data);
}

static bool valid_extension(const char *ext)
{
	dstr test = {0};
	bool valid = false;
	const char *b;
	const char *e;

	if (!ext || !*ext)
		return false;

	b = &EXTENSIONS_MEDIA[1];
	e = strchr(b, ';');

	for (;;) {
		if (e)
			dstr_ncopy(&test, b, e - b);
		else
			dstr_copy(&test, b);

		if (dstr_cmpi(&test, ext) == 0) {
			valid = true;
			break;
		}

		if (!e)
			break;

		b = e + 2;
		e = strchr(b, ';');
	}

	dstr_free(&test);
	return valid;
}

static std::string to_lower(std::string str)
{
	std::string result;
	result.resize(str.length());

	std::transform(str.begin(), str.end(), result.begin(),
		       [](unsigned char c) { return std::tolower(c); });
	return result;
}
