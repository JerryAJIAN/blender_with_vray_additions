/*
 * Copyright (c) 2015, Chaos Software Ltd
 *
 * V-Ray For Blender
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "vfb_plugin_exporter_zmq.h"
#include "vfb_export_settings.h"
#include "vfb_params_json.h"
#include "vfb_log.h"

#include "BLI_utildefines.h"

#include <boost/filesystem.hpp>

#include <limits>

namespace fs = boost::filesystem;

using namespace VRayForBlender;

std::mutex ZmqServer::clientMtx;
ClientPtr ZmqServer::serverCheck;

bool ZmqServer::isRunning() {
	std::lock_guard<std::mutex> lock(clientMtx);
	return serverCheck && serverCheck->good() && serverCheck->connected();
}

bool ZmqServer::start(const char * addr) {
	std::lock_guard<std::mutex> lock(clientMtx);

	if (!serverCheck) {
		getLog().info("Starting heartbeat client for %s", addr);
		serverCheck.reset(new ZmqClient(true));
		serverCheck->connect(addr);
		if (serverCheck->connected()) {
			return true;
		}
	} else {
		getLog().error("Heartbeat client already running...");
		// return true as we are running good.
		if (serverCheck->good() && serverCheck->connected()) {
			return true;
		}
	}

	return false;
}

bool ZmqServer::stop() {
	std::lock_guard<std::mutex> lock(clientMtx);

	if (serverCheck) {
		getLog().info("Stopping hearbeat client... ");
		if (serverCheck->good() && serverCheck->connected()) {
			serverCheck->stopServer();
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		serverCheck->syncStop();
		serverCheck.reset();
		getLog().info("... done.");
		return true;
	}

	getLog().error("No zmq heartbeat client running...");
	return false;
}



void ZmqExporter::ZmqRenderImage::update(const VRayBaseTypes::AttrImage &img, ZmqExporter * exp, bool fixImage) {
	// convertions here should match the blender's render pass channel requirements

	if (img.imageType == VRayBaseTypes::AttrImage::ImageType::RGBA_REAL && img.isBucket()) {
		// merge in the bucket

		if (!pixels) {
			std::unique_lock<std::mutex> lock(exp->m_imgMutex);
			if (!pixels) {
				w = exp->m_cachedValues.renderWidth;
				h = exp->m_cachedValues.renderHeight;
				channels = 4;

				pixels = new float[w * h * channels];
				memset(pixels, 0, w * h * channels * sizeof(float));

				resetUpdated();
			}
		}

		fixImage = false;
		const float * sourceImage = reinterpret_cast<const float *>(img.data.get());

		updateRegion(sourceImage, {img.x, img.y, img.width, img.height});

	} else if (img.imageType == VRayBaseTypes::AttrImage::ImageType::JPG) {
		int channels = 0;
		float * imgData = jpegToPixelData(reinterpret_cast<unsigned char*>(img.data.get()), img.size, channels);

		{
			std::lock_guard<std::mutex> lock(exp->m_imgMutex);

			this->channels = channels;
			this->w = img.width;
			this->h = img.height;
			delete[] pixels;
			this->pixels = imgData;
		}
	} else if (img.imageType == VRayBaseTypes::AttrImage::ImageType::RGBA_REAL ||
		       img.imageType == VRayBaseTypes::AttrImage::ImageType::RGB_REAL ||
		       img.imageType == VRayBaseTypes::AttrImage::ImageType::BW_REAL) {

		const float * imgData = reinterpret_cast<const float *>(img.data.get());
		float * myImage = nullptr;
		int channels = 0;

		switch (img.imageType) {
		case VRayBaseTypes::AttrImage::ImageType::RGBA_REAL:
			channels = 4;
			myImage = new float[img.width * img.height * channels];
			memcpy(myImage, imgData, img.width * img.height * channels * sizeof(float));

			break;
		case VRayBaseTypes::AttrImage::ImageType::RGB_REAL:
			channels = 3;
			myImage = new float[img.width * img.height * channels];

			for (int c = 0; c < img.width * img.height; ++c) {
				const float * source = imgData + (c * 4);
				float * dest = myImage + (c * channels);

				dest[0] = source[0];
				dest[1] = source[1];
				dest[2] = source[2];
			}

			break;
		case VRayBaseTypes::AttrImage::ImageType::BW_REAL:
			channels = 1;
			myImage = new float[img.width * img.height * channels];

			for (int c = 0; c < img.width * img.height; ++c) {
				const float * source = imgData + (c * 4);
				float * dest = myImage + (c * channels);

				dest[0] = source[0];
			}

			break;
		default:
			getLog().warning("MISSING IMAGE FORMAT CONVERTION FOR %d", img.imageType);
		}

		{
			std::lock_guard<std::mutex> lock(exp->m_imgMutex);
			this->channels = channels;
			this->w = img.width;
			this->h = img.height;
			delete[] pixels;
			this->pixels = myImage;
		}
	}

	if (fixImage) {
		flip();
		resetAlpha();
		clamp(1.0f, 1.0f);
	}
}


ZmqExporter::ZmqExporter(const ExporterSettings & settings)
    : PluginExporter(settings)
    , m_client(nullptr)
    , m_isDirty(true)
    , m_isAborted(false)
    , m_started(false)
    , m_exportedCount(0)
{
	checkZmqClient();
}


ZmqExporter::~ZmqExporter()
{
	free();

	{
		std::lock_guard<std::mutex> lock(m_zmqClientMutex);
		m_client->setCallback([](const VRayMessage &, ZmqClient *) {});
		m_client.reset();
	}

	// we could be destroyed while someone is inside get_render_channel and is accessing m_LayerImges
	// but we can't protect it from inside this class
}

RenderImage ZmqExporter::get_render_channel(RenderChannelType channelType) {
	RenderImage img;

	auto imgIter = m_layerImages.find(channelType);
	if (imgIter != m_layerImages.end()) {
		std::unique_lock<std::mutex> lock(m_imgMutex);
		imgIter = m_layerImages.find(channelType);

		if (imgIter != m_layerImages.end()) {
			RenderImage &storedImage = imgIter->second;
			if (storedImage.pixels) {
				img = std::move(RenderImage::deepCopy(storedImage));
			}
		}
	}
	return img;
}

RenderImage ZmqExporter::get_image() {
	return get_render_channel(RenderChannelType::RenderChannelTypeNone);
}

enum MessageLevel {
	MessageError = 9999,
	MessageWarning = 19999,
	MessageInfo = 29999
};

void ZmqExporter::zmqCallback(const VRayMessage & message, ZmqClient *) {
	const auto msgType = message.getType();
	if (msgType == VRayMessage::Type::VRayLog) {
		std::string msg = *message.getValue<AttrString>();
		const auto newLine = msg.find_first_of("\n\r");
		if (newLine != std::string::npos) {
			msg.resize(newLine);
		}

		LogLevel msgLevel = LogLevel::debug;
		const int logLevel = message.getLogLevel();
		if (logLevel <= MessageError) {
			msgLevel = LogLevel::error;
		}
		else if (logLevel > MessageError && logLevel <= MessageWarning) {
			msgLevel = LogLevel::warning;
		}
		else if (logLevel > MessageWarning && logLevel <= MessageInfo) {
			msgLevel = LogLevel::info;
		}

		getLog().log(msgLevel, msg.c_str());

		if (callback_on_message_update) {
			std::string guiMsg("V-Ray: ");
			guiMsg.append(msg);
			callback_on_message_update("", guiMsg.c_str());
		}
	} else if (msgType == VRayMessage::Type::Image) {
		auto * set = message.getValue<VRayBaseTypes::AttrImageSet>();
		bool ready = set->sourceType == VRayBaseTypes::ImageSourceType::ImageReady;
		bool rtImageUpdate = false;
		for (const auto &img : set->images) {
			m_layerImages[img.first].update(img.second, this, !is_viewport);
			// for result buckets use on bucket ready, otherwise rt image updated callback
			if (img.first == RenderChannelType::RenderChannelTypeNone && img.second.isBucket() && this->callback_on_bucket_ready) {
				this->callback_on_bucket_ready(img.second);
			} else {
				rtImageUpdate = true;
			}
		}

		if (rtImageUpdate && this->callback_on_rt_image_updated) {
			callback_on_rt_image_updated.cb();
		}

		if (ready && this->callback_on_image_ready) {
			this->callback_on_image_ready.cb();
		}

	} else if (msgType == VRayMessage::Type::ChangeRenderer) {
		if (message.getRendererAction() == VRayMessage::RendererAction::SetRendererState) {
			m_isAborted = false;
			switch (message.getRendererState()) {
			case VRayMessage::RendererState::Abort:
				m_isAborted = true;
				break;
			case VRayMessage::RendererState::Progress:
				render_progress = *message.getValue<VRayBaseTypes::AttrSimpleType<float>>();
				break;
			case VRayMessage::RendererState::ProgressMessage:
				progress_message = *message.getValue<VRayBaseTypes::AttrSimpleType<std::string>>();
				break;
			case VRayMessage::RendererState::Continue:
				this->last_rendered_frame = *message.getValue<VRayBaseTypes::AttrSimpleType<float>>();
				break;
			default:
				VFB_Assert(!"Receieved unexpected RendererState message from renderer.");
			}
		}
	}
}

void ZmqExporter::init()
{
	try {
		getLog().info("Initing ZmqExporter");
		using std::placeholders::_1;
		using std::placeholders::_2;

		m_client->setCallback(std::bind(&ZmqExporter::zmqCallback, this, _1, _2));

		if (!m_client->connected()) {
			char portStr[32];
			snprintf(portStr, 32, ":%d", exporter_settings.zmq_server_port);
			const std::string addr = exporter_settings.zmq_server_address.empty() ? "127.0.0.1" : exporter_settings.zmq_server_address;
			m_client->connect(("tcp://" + addr + portStr).c_str());
		}

		if (m_client->connected()) {
			VRayMessage::RendererType type = VRayMessage::RendererType::None;
			if (exporter_settings.is_preview) {
				type = VRayMessage::RendererType::Preview;
			} else if (is_viewport) {
				type = VRayMessage::RendererType::RT;
			} else {
				if (exporter_settings.settings_animation.use) {
					type = VRayMessage::RendererType::Animation;
				} else {
					type = VRayMessage::RendererType::SingleFrame;
				}
			}

			VRayMessage::DRFlags drflags = VRayMessage::DRFlags::None;
			if (exporter_settings.settings_dr.use) {
				drflags = VRayMessage::DRFlags::EnableDr;
				if (exporter_settings.settings_dr.renderOnlyOnHodes) {
					drflags = static_cast<VRayMessage::DRFlags>(static_cast<int>(VRayMessage::DRFlags::RenderOnlyOnHosts) | static_cast<int>(drflags));
				}
			}

			m_client->send(VRayMessage::msgRendererActionInit(type, drflags));
			m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::SetRenderMode, static_cast<int>(exporter_settings.render_mode)));

			m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::GetImage, static_cast<int>(RenderChannelType::RenderChannelTypeNone)));
			if (!is_viewport && !exporter_settings.settings_animation.use) {
				m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::GetImage, static_cast<int>(RenderChannelType::RenderChannelTypeVfbRealcolor)));
			}

			m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::SetVfbShow, exporter_settings.show_vfb));
			m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::SetQuality, exporter_settings.viewport_image_quality));
			m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::SetViewportImageFormat, static_cast<int>(exporter_settings.viewport_image_type)));

			if (exporter_settings.settings_dr.use) { 
				const std::vector<std::string> & hostItems = exporter_settings.settings_dr.hosts;
				std::string hostsStr;
				hostsStr.reserve(hostItems.size() * 24); // 24 chars per host is enough - e.g 123.123.123.123:12345;
				for (const std::string & host : hostItems) {
					hostsStr += host;
					hostsStr.push_back(';');
				}
				hostsStr.pop_back(); // remove last delimiter - ;
				m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::ResetsHosts, hostsStr));
			}

			m_cachedValues.show_vfb = exporter_settings.show_vfb;
			m_cachedValues.viewport_image_quality = exporter_settings.viewport_image_quality;
			m_cachedValues.viewport_image_type = exporter_settings.viewport_image_type;
			m_cachedValues.renderHeight = 0;
			m_cachedValues.renderWidth = 0;
			m_cachedValues.render_mode = exporter_settings.render_mode;
		}
	} catch (zmq::error_t &e) {
		getLog().error("Failed to initialize ZMQ client\n%s", e.what());
	}
}

void ZmqExporter::checkZmqClient()
{
	std::lock_guard<std::mutex> lock(m_zmqClientMutex);

	if (!m_client) {
		m_client = ClientPtr(new ZmqClient());
	} else {
		if (!m_client->connected()) {
			m_isAborted = true;
			// we can't connect dont retry
			return;
		}

		if (!m_client->good()) {
			m_isAborted = true;
			VFB_Assert(!"ZMQ client disconnected from server!");
		}
	}
}

void ZmqExporter::free()
{
	checkZmqClient();
	m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::Free));
}

void ZmqExporter::clear_frame_data(float upTo)
{
	checkZmqClient();
	m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::ClearFrameValues, upTo));
}

void ZmqExporter::wait_for_server()
{
	checkZmqClient();
	m_client->waitForMessages();
}

void ZmqExporter::sync()
{
	PluginExporter::sync();
#define CHECK_UPDATE(name, upd)\
	if (m_cachedValues.name != exporter_settings.name) {\
		upd;\
		m_cachedValues.name = exporter_settings.name;\
	}

	checkZmqClient();
	CHECK_UPDATE(show_vfb, m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::SetVfbShow, exporter_settings.show_vfb)));
	CHECK_UPDATE(viewport_image_quality, m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::SetQuality, exporter_settings.viewport_image_quality)));
	CHECK_UPDATE(viewport_image_type, m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::SetViewportImageFormat, static_cast<int>(exporter_settings.viewport_image_type))));
	CHECK_UPDATE(render_mode, m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::SetRenderMode, static_cast<int>(exporter_settings.render_mode))));
#undef CHECK_UPDATE
	// call commit explicitly else will often commit before calling startSync which is not needed
	// set_commit_state(CommitAction::CommitNow);
}

void ZmqExporter::set_current_frame(float frame)
{
	if (frame != current_scene_frame) {
		current_scene_frame = frame;
		checkZmqClient();
		m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::SetCurrentFrame, frame));
	}
}

void ZmqExporter::set_render_region(int x, int y, int w, int h, bool crop)
{
	checkZmqClient();
	const AttrListInt region({x, y, w, h});
	if (crop) {
		m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::SetCropRegion, region));
	} else {
		m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::SetRenderRegion, region));
	}
}

void ZmqExporter::set_render_size(const int &w, const int &h)
{
	std::unique_lock<std::mutex> lock(m_imgMutex);
	if (w != m_cachedValues.renderWidth || h != m_cachedValues.renderHeight) {
		m_cachedValues.renderWidth = w;
		m_cachedValues.renderHeight = h;
		checkZmqClient();
		m_client->send(VRayMessage::msgRendererResize(w, h));
	}
}

void ZmqExporter::set_camera_plugin(const std::string &pluginName)
{
	if (m_cachedValues.activeCamera != pluginName) {
		m_isDirty = true;
		checkZmqClient();
		m_cachedValues.activeCamera = pluginName;
		m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::SetCurrentCamera, pluginName));
	}
}

void ZmqExporter::set_commit_state(VRayBaseTypes::CommitAction ca)
{
	if (ca == CommitAction::CommitAutoOn || ca == CommitAction::CommitAutoOff) {
		if (ca != commit_state) {
			commit_state = ca;
			checkZmqClient();
			m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::SetCommitAction, static_cast<int>(ca)));
		}
	} else {
		if (m_isDirty) {
			checkZmqClient();
			m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::SetCommitAction, static_cast<int>(ca)));
			m_isDirty = false;
		}
	}
}

void ZmqExporter::start()
{
	checkZmqClient();
	m_started = true;
	m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::Start));
}

void ZmqExporter::reset()
{
	// TODO: try with clear values up to time
	m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::Reset));

	m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::SetVfbShow, exporter_settings.show_vfb));
	m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::SetQuality, exporter_settings.viewport_image_quality));
	m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::SetViewportImageFormat, static_cast<int>(exporter_settings.viewport_image_type)));
	m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::SetRenderMode, static_cast<int>(exporter_settings.render_mode)));

	m_cachedValues = {}; // reset cache
}

void ZmqExporter::stop()
{
	m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::Stop));
}

void ZmqExporter::export_vrscene(const std::string &filepath)
{
	if (exporter_settings.settings_files.use_separate) {
		getLog().warning("ZMQ will ignore option \"Separate Files\" and export in one file!");
	}
	fs::path dirPath(filepath);
	dirPath.remove_filename();

	boost::system::error_code code;
	if (!fs::exists(dirPath) && !fs::create_directories(dirPath, code)) {
		getLog().error("Failed to create directory \"%s\": %s", filepath.c_str(), code.message().c_str());
	} else {
		checkZmqClient();
		m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::ExportScene, filepath));
		m_client->waitForMessages();
	}
}

int ZmqExporter::remove_plugin_impl(const std::string &name)
{
	m_isDirty = true;
	checkZmqClient();
	m_client->send(VRayMessage::msgPluginAction(name, VRayMessage::PluginAction::Remove));
	return PluginExporter::remove_plugin_impl(name);
}

void ZmqExporter::replace_plugin(const std::string & oldPlugin, const std::string & newPlugin)
{
	m_isDirty = true;
	checkZmqClient();
	m_client->send(VRayMessage::msgPluginReplace(oldPlugin, newPlugin));
}


AttrPlugin ZmqExporter::export_plugin_impl(const PluginDesc & pluginDesc)
{
	m_isDirty = true;
	checkZmqClient();

	if (pluginDesc.pluginID.empty()) {
		getLog().warning("[%s] PluginDesc.pluginID is not set!",
			pluginDesc.pluginName.c_str());
		return AttrPlugin();
	}
	++m_exportedCount;

	const std::string & name = pluginDesc.pluginName;
	AttrPlugin plugin(name);

	const auto pluginType = GetPluginDescription(pluginDesc.pluginID).pluginType;
	if (pluginType == ParamDesc::PluginType::PluginChannel) {
		static const std::pair<std::string, RenderChannelType> channelMap[] = {
			{"RenderChannelBumpNormals", RenderChannelType::RenderChannelTypeVfbBumpnormal},
			{"RenderChannelColor", RenderChannelType::RenderChannelTypeVfbColor},
			{"RenderChannelDenoiser", RenderChannelType::RenderChannelTypeVfbDenoised},
			{"RenderChannelDRBucket", RenderChannelType::RenderChannelTypeDrbucket},
			{"RenderChannelNodeID", RenderChannelType::RenderChannelTypeVfbNodeid},
			{"RenderChannelNormals", RenderChannelType::RenderChannelTypeVfbNormal},
			{"RenderChannelRenderID", RenderChannelType::RenderChannelTypeVfbRenderID},
			{"RenderChannelVelocity", RenderChannelType::RenderChannelTypeVfbVelocity},
			{"RenderChannelZDepth", RenderChannelType::RenderChannelTypeVfbZdepth},
		};

		for (int c = 0; c < sizeof(channelMap) / sizeof(channelMap[0]); ++c) {
			if (pluginDesc.pluginID == channelMap[c].first) {
				m_client->send(VRayMessage::msgRendererAction(VRayMessage::RendererAction::GetImage, static_cast<int>(channelMap[c].second)));
			}
		}
	}

	m_client->send(VRayMessage::msgPluginCreate(name, pluginDesc.pluginID));

	for (auto & attributePairs : pluginDesc.pluginAttrs) {
		const PluginAttr & attr = attributePairs.second;
		if (attr.attrValue.getType() != ValueTypeUnknown) {
			m_client->send(VRayMessage::msgPluginSetProperty(name, attr.attrName, attr.attrValue));
		}
	}

	return plugin;
}

int ZmqExporter::getExportedPluginsCount() const
{
	return m_exportedCount;
}

void ZmqExporter::resetExportedPluginsCount()
{
	m_exportedCount = 0;
}
