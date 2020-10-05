/***********************************************************************************************************************
*                                                                                                                      *
* ANTIKERNEL v0.1                                                                                                      *
*                                                                                                                      *
* Copyright (c) 2012-2020 Andrew D. Zonenberg                                                                          *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@author Andrew D. Zonenberg
	@brief  Implementation of WaveformArea
 */
#include "glscopeclient.h"
#include "WaveformArea.h"
#include "OscilloscopeWindow.h"
#include <random>
#include "../../lib/scopeprotocols/scopeprotocols.h"

using namespace std;
using namespace glm;

bool WaveformArea::m_isGlewInitialized = false;

WaveformArea::WaveformArea(
	StreamDescriptor channel,
	OscilloscopeWindow* parent
	)
	: m_persistence(false)
	, m_channel(channel)
	, m_parent(parent)
	, m_pixelsPerVolt(1)
	, m_axisLabelFont("monospace normal 10")
	, m_infoBoxFont("sans normal 10")
	, m_cursorLabelFont("sans normal 10")
	, m_decodeFont("sans normal 10")
{
	m_axisLabelFont.set_weight(Pango::WEIGHT_NORMAL);
	m_infoBoxFont.set_weight(Pango::WEIGHT_NORMAL);
	m_cursorLabelFont.set_weight(Pango::WEIGHT_NORMAL);
	m_decodeFont.set_weight(Pango::WEIGHT_NORMAL);

	SharedCtorInit();
}

/**
	@brief Semi-copy constructor, used when copying a waveform to a new group

	Note that we only clone UI settings, the GL context, GTK properties, etc are new!
 */
WaveformArea::WaveformArea(const WaveformArea* clone)
	: m_persistence(clone->m_persistence)
	, m_channel(clone->m_channel)
	, m_parent(clone->m_parent)
	, m_pixelsPerVolt(clone->m_pixelsPerVolt)
	, m_axisLabelFont(clone->m_axisLabelFont)
	, m_infoBoxFont(clone->m_infoBoxFont)
	, m_cursorLabelFont(clone->m_cursorLabelFont)
	, m_decodeFont(clone->m_decodeFont)
{
	SharedCtorInit();
}

void WaveformArea::SharedCtorInit()
{
	m_updatingContextMenu 	= false;
	m_selectedChannel		= m_channel;
	m_dragState 			= DRAG_NONE;
	m_insertionBarLocation	= INSERT_NONE;
	m_dropTarget			= NULL;
	m_padding 				= 2;
	m_overlaySpacing		= 30;
	m_persistenceClear 		= true;
	m_firstFrame 			= false;
	m_waveformRenderData	= NULL;
	m_dragOverlayPosition	= 0;
	m_geometryDirty			= false;
	m_positionDirty			= false;
	m_mouseElementPosition	= LOC_PLOT;

	m_plotRight = 1;
	m_width		= 1;
	m_height 	= 1;

	m_decodeDialog 			= NULL;
	m_pendingDecode			= NULL;

	//Configure the OpenGL context we want
	set_has_alpha();
	set_has_depth_buffer(false);
	set_has_stencil_buffer(false);
	set_required_version(4, 2);
	set_use_es(false);

	add_events(
		Gdk::EXPOSURE_MASK |
		Gdk::POINTER_MOTION_MASK |
		Gdk::SCROLL_MASK |
		Gdk::BUTTON_PRESS_MASK |
		Gdk::BUTTON_RELEASE_MASK);

	CreateWidgets();

	m_group = NULL;

	m_channel.m_channel->AddRef();
}

WaveformArea::~WaveformArea()
{
	m_channel.m_channel->Release();

	for(auto d : m_overlays)
		OnRemoveOverlay(d);
	m_overlays.clear();

	if(m_decodeDialog)
		delete m_decodeDialog;
	if(m_pendingDecode)
		delete m_pendingDecode;

	for(auto m : m_moveExistingGroupItems)
	{
		m_moveMenu.remove(*m);
		delete m;
	}
	m_moveExistingGroupItems.clear();
}

void WaveformArea::OnRemoveOverlay(StreamDescriptor filter)
{
	//Remove the render data for it
	auto it = m_overlayRenderData.find(filter);
	if(it != m_overlayRenderData.end())
		m_overlayRenderData.erase(it);

	filter.m_channel->Release();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Initialization

void WaveformArea::CreateWidgets()
{
	//Delete
	auto item = Gtk::manage(new Gtk::MenuItem("Delete", false));
		item->signal_activate().connect(
			sigc::mem_fun(*this, &WaveformArea::OnHide));
		m_contextMenu.append(*item);

	//Move/copy
	m_contextMenu.append(m_moveItem);
		m_moveItem.set_label("Move waveform to");
		m_moveItem.set_submenu(m_moveMenu);
			m_moveMenu.append(m_moveNewGroupBelowItem);
				m_moveNewGroupBelowItem.set_label("Insert new group at bottom");
				m_moveNewGroupBelowItem.signal_activate().connect(
					sigc::mem_fun(*this, &WaveformArea::OnMoveNewBelow));
			m_moveMenu.append(m_moveNewGroupRightItem);
				m_moveNewGroupRightItem.set_label("Insert new group at right");
				m_moveNewGroupRightItem.signal_activate().connect(
					sigc::mem_fun(*this, &WaveformArea::OnMoveNewRight));
			m_moveMenu.append(*Gtk::manage(new Gtk::SeparatorMenuItem));
	m_contextMenu.append(m_copyItem);
		m_copyItem.set_label("Copy waveform to");
		m_copyItem.set_submenu(m_copyMenu);
			m_copyMenu.append(m_copyNewGroupBelowItem);
				m_copyNewGroupBelowItem.set_label("Insert new group at bottom");
				m_copyNewGroupBelowItem.signal_activate().connect(
					sigc::mem_fun(*this, &WaveformArea::OnCopyNewBelow));
			m_copyMenu.append(m_copyNewGroupRightItem);
				m_copyNewGroupRightItem.set_label("Insert new group at right");
				m_copyNewGroupRightItem.signal_activate().connect(
					sigc::mem_fun(*this, &WaveformArea::OnCopyNewRight));

	//Persistence
	m_contextMenu.append(m_persistenceItem);
		m_persistenceItem.set_label("Persistence");
		m_persistenceItem.signal_activate().connect(
			sigc::mem_fun(*this, &WaveformArea::OnTogglePersistence));

	m_contextMenu.append(*Gtk::manage(new Gtk::SeparatorMenuItem));

	//Cursor
	m_contextMenu.append(m_cursorItem);
		m_cursorItem.set_label("Cursor");
		m_cursorItem.set_submenu(m_cursorMenu);
			m_cursorMenu.append(m_cursorNoneItem);
				m_cursorNoneItem.set_label("None");
				m_cursorNoneItem.set_group(m_cursorGroup);
				m_cursorNoneItem.signal_activate().connect(
					sigc::bind<WaveformGroup::CursorConfig, Gtk::RadioMenuItem*>(
						sigc::mem_fun(*this, &WaveformArea::OnCursorConfig),
						WaveformGroup::CURSOR_NONE,
						&m_cursorNoneItem));
			m_cursorMenu.append(m_cursorSingleVerticalItem);
				m_cursorSingleVerticalItem.set_label("Vertical (single)");
				m_cursorSingleVerticalItem.set_group(m_cursorGroup);
				m_cursorSingleVerticalItem.signal_activate().connect(
					sigc::bind<WaveformGroup::CursorConfig, Gtk::RadioMenuItem*>(
						sigc::mem_fun(*this, &WaveformArea::OnCursorConfig),
						WaveformGroup::CURSOR_X_SINGLE,
						&m_cursorSingleVerticalItem));
			m_cursorMenu.append(m_cursorDualVerticalItem);
				m_cursorDualVerticalItem.set_label("Vertical (dual)");
				m_cursorDualVerticalItem.set_group(m_cursorGroup);
				m_cursorDualVerticalItem.signal_activate().connect(
					sigc::bind<WaveformGroup::CursorConfig, Gtk::RadioMenuItem*>(
						sigc::mem_fun(*this, &WaveformArea::OnCursorConfig),
						WaveformGroup::CURSOR_X_DUAL,
						&m_cursorDualVerticalItem));

	m_contextMenu.append(*Gtk::manage(new Gtk::SeparatorMenuItem));

	//Trigger
	m_contextMenu.append(m_triggerItem);
		m_triggerItem.set_label("Trigger");
		m_triggerItem.set_submenu(m_triggerMenu);
			m_risingTriggerItem.set_label("Rising edge");
			m_risingTriggerItem.signal_activate().connect(
				sigc::bind<EdgeTrigger::EdgeType, Gtk::RadioMenuItem*>(
					sigc::mem_fun(*this, &WaveformArea::OnTriggerMode),
					EdgeTrigger::EDGE_RISING,
					&m_risingTriggerItem));
			m_risingTriggerItem.set_group(m_triggerGroup);
			m_triggerMenu.append(m_risingTriggerItem);

			m_fallingTriggerItem.set_label("Falling edge");
			m_fallingTriggerItem.signal_activate().connect(
				sigc::bind<EdgeTrigger::EdgeType, Gtk::RadioMenuItem*>(
					sigc::mem_fun(*this, &WaveformArea::OnTriggerMode),
					EdgeTrigger::EDGE_FALLING,
					&m_fallingTriggerItem));
			m_fallingTriggerItem.set_group(m_triggerGroup);
			m_triggerMenu.append(m_fallingTriggerItem);

			m_bothTriggerItem.set_label("Both edges");
			m_bothTriggerItem.signal_activate().connect(
				sigc::bind<EdgeTrigger::EdgeType, Gtk::RadioMenuItem*>(
					sigc::mem_fun(*this, &WaveformArea::OnTriggerMode),
					EdgeTrigger::EDGE_ANY,
					&m_bothTriggerItem));
			m_bothTriggerItem.set_group(m_triggerGroup);
			m_triggerMenu.append(m_bothTriggerItem);

	m_contextMenu.append(*Gtk::manage(new Gtk::SeparatorMenuItem));

	//Attenuation
	m_contextMenu.append(m_attenItem);
		m_attenItem.set_label("Attenuation");
		m_attenItem.set_submenu(m_attenMenu);
			m_atten1xItem.set_label("1x");
				m_atten1xItem.set_group(m_attenGroup);
				m_atten1xItem.signal_activate().connect(sigc::bind<double, Gtk::RadioMenuItem*>(
					sigc::mem_fun(*this, &WaveformArea::OnAttenuation), 1, &m_atten1xItem));
				m_attenMenu.append(m_atten1xItem);
			m_atten10xItem.set_label("10x");
				m_atten10xItem.set_group(m_attenGroup);
				m_atten10xItem.signal_activate().connect(sigc::bind<double, Gtk::RadioMenuItem*>(
					sigc::mem_fun(*this, &WaveformArea::OnAttenuation), 10, &m_atten10xItem));
				m_attenMenu.append(m_atten10xItem);
			m_atten20xItem.set_label("20x");
				m_atten20xItem.set_group(m_attenGroup);
				m_atten20xItem.signal_activate().connect(sigc::bind<double, Gtk::RadioMenuItem*>(
					sigc::mem_fun(*this, &WaveformArea::OnAttenuation), 20, &m_atten20xItem));
				m_attenMenu.append(m_atten20xItem);

	//Bandwidth
	m_contextMenu.append(m_bwItem);
		m_bwItem.set_label("Bandwidth");
		m_bwItem.set_submenu(m_bwMenu);
			m_bwFullItem.set_label("Full");
				m_bwFullItem.set_group(m_bwGroup);
				m_bwFullItem.signal_activate().connect(sigc::bind<int, Gtk::RadioMenuItem*>(
					sigc::mem_fun(*this, &WaveformArea::OnBandwidthLimit), 0, &m_bwFullItem));
				m_bwMenu.append(m_bwFullItem);
			m_bw200Item.set_label("200 MHz");
				m_bw200Item.set_group(m_bwGroup);
				m_bw200Item.signal_activate().connect(sigc::bind<int, Gtk::RadioMenuItem*>(
					sigc::mem_fun(*this, &WaveformArea::OnBandwidthLimit), 200, &m_bw200Item));
				m_bwMenu.append(m_bw200Item);
			m_bw20Item.set_label("20 MHz");
				m_bw20Item.set_group(m_bwGroup);
				m_bw20Item.signal_activate().connect(sigc::bind<int, Gtk::RadioMenuItem*>(
					sigc::mem_fun(*this, &WaveformArea::OnBandwidthLimit), 20, &m_bw20Item));
				m_bwMenu.append(m_bw20Item);

	//Coupling
	m_contextMenu.append(m_couplingItem);
		m_couplingItem.set_label("Coupling");
		m_couplingItem.set_submenu(m_couplingMenu);
			m_ac1MCouplingItem.set_label("AC 1M");
				m_ac1MCouplingItem.set_group(m_couplingGroup);
				m_ac1MCouplingItem.signal_activate().connect(
					sigc::bind<OscilloscopeChannel::CouplingType, Gtk::RadioMenuItem*>(
						sigc::mem_fun(*this, &WaveformArea::OnCoupling),
						OscilloscopeChannel::COUPLE_AC_1M, &m_ac1MCouplingItem));
				m_couplingMenu.append(m_ac1MCouplingItem);
			m_dc1MCouplingItem.set_label("DC 1M");
				m_dc1MCouplingItem.set_group(m_couplingGroup);
				m_dc1MCouplingItem.signal_activate().connect(
					sigc::bind<OscilloscopeChannel::CouplingType, Gtk::RadioMenuItem*>(
						sigc::mem_fun(*this, &WaveformArea::OnCoupling),
						OscilloscopeChannel::COUPLE_DC_1M, &m_dc1MCouplingItem));
				m_couplingMenu.append(m_dc1MCouplingItem);
			m_dc50CouplingItem.set_label("DC 50Ω");
				m_dc50CouplingItem.set_group(m_couplingGroup);
				m_dc50CouplingItem.signal_activate().connect(
					sigc::bind<OscilloscopeChannel::CouplingType, Gtk::RadioMenuItem*>(
						sigc::mem_fun(*this, &WaveformArea::OnCoupling),
						OscilloscopeChannel::COUPLE_DC_50, &m_dc50CouplingItem));
				m_couplingMenu.append(m_dc50CouplingItem);
			m_gndCouplingItem.set_label("GND");
				m_gndCouplingItem.set_group(m_couplingGroup);
				m_gndCouplingItem.signal_activate().connect(
					sigc::bind<OscilloscopeChannel::CouplingType, Gtk::RadioMenuItem*>(
						sigc::mem_fun(*this, &WaveformArea::OnCoupling),
						OscilloscopeChannel::COUPLE_GND, &m_gndCouplingItem));
				m_couplingMenu.append(m_gndCouplingItem);

	m_contextMenu.append(*Gtk::manage(new Gtk::SeparatorMenuItem));

	//Decode
	m_contextMenu.append(m_decodeAlphabeticalItem);
		m_decodeAlphabeticalItem.set_label("Alphabetical");
		m_decodeAlphabeticalItem.set_submenu(m_decodeAlphabeticalMenu);
	m_contextMenu.append(m_decodeBusItem);
		m_decodeBusItem.set_label("Buses");
		m_decodeBusItem.set_submenu(m_decodeBusMenu);
	m_contextMenu.append(m_decodeClockItem);
		m_decodeClockItem.set_label("Clocking");
		m_decodeClockItem.set_submenu(m_decodeClockMenu);
	m_contextMenu.append(m_decodeMathItem);
		m_decodeMathItem.set_label("Math");
		m_decodeMathItem.set_submenu(m_decodeMathMenu);
	m_contextMenu.append(m_decodeMeasurementItem);
		m_decodeMeasurementItem.set_label("Measurement");
		m_decodeMeasurementItem.set_submenu(m_decodeMeasurementMenu);
	m_contextMenu.append(m_decodeMemoryItem);
		m_decodeMemoryItem.set_label("Memory");
		m_decodeMemoryItem.set_submenu(m_decodeMemoryMenu);
	m_contextMenu.append(m_decodeMiscItem);
		m_decodeMiscItem.set_label("Misc");
		m_decodeMiscItem.set_submenu(m_decodeMiscMenu);
	m_contextMenu.append(m_decodePowerItem);
		m_decodePowerItem.set_label("Power");
		m_decodePowerItem.set_submenu(m_decodePowerMenu);
	m_contextMenu.append(m_decodeRFItem);
		m_decodeRFItem.set_label("RF");
		m_decodeRFItem.set_submenu(m_decodeRFMenu);
	m_contextMenu.append(m_decodeSerialItem);
		m_decodeSerialItem.set_label("Serial");
		m_decodeSerialItem.set_submenu(m_decodeSerialMenu);
	m_contextMenu.append(m_decodeSignalIntegrityItem);
		m_decodeSignalIntegrityItem.set_label("Signal Integrity");
		m_decodeSignalIntegrityItem.set_submenu(m_decodeSignalIntegrityMenu);


		vector<string> names;
		Filter::EnumProtocols(names);
		for(auto p : names)
		{
			item = Gtk::manage(new Gtk::MenuItem(p, false));
			item->signal_activate().connect(
				sigc::bind<string>(sigc::mem_fun(*this, &WaveformArea::OnProtocolDecode), p));

			//Create a test decode and see where it goes
			auto d = Filter::CreateFilter(p, "");
			switch(d->GetCategory())
			{
				case Filter::CAT_ANALYSIS:
					m_decodeSignalIntegrityMenu.append(*item);
					break;

				case Filter::CAT_BUS:
					m_decodeBusMenu.append(*item);
					break;

				case Filter::CAT_CLOCK:
					m_decodeClockMenu.append(*item);
					break;

				case Filter::CAT_POWER:
					m_decodePowerMenu.append(*item);
					break;

				case Filter::CAT_RF:
					m_decodeRFMenu.append(*item);
					break;

				case Filter::CAT_MEASUREMENT:
					m_decodeMeasurementMenu.append(*item);
					break;

				case Filter::CAT_MATH:
					m_decodeMathMenu.append(*item);
					break;

				case Filter::CAT_MEMORY:
					m_decodeMemoryMenu.append(*item);
					break;

				case Filter::CAT_SERIAL:
					m_decodeSerialMenu.append(*item);
					break;

				default:
				case Filter::CAT_MISC:
					m_decodeMiscMenu.append(*item);
					break;
			}
			delete d;

			//Make a second menu item and put on the alphabetical list
			item = Gtk::manage(new Gtk::MenuItem(p, false));
			item->signal_activate().connect(
				sigc::bind<string>(sigc::mem_fun(*this, &WaveformArea::OnProtocolDecode), p));
			m_decodeAlphabeticalMenu.append(*item);
		}

	//TODO: delete measurements once we get rid of them all
	m_contextMenu.append(*Gtk::manage(new Gtk::SeparatorMenuItem));

	//Statistics
	m_contextMenu.append(m_statisticsItem);
		m_statisticsItem.set_label("Statistics");
		m_statisticsItem.signal_activate().connect(
			sigc::mem_fun(*this, &WaveformArea::OnStatistics));


	m_contextMenu.show_all();
}

void WaveformArea::on_realize()
{
	//Let the base class create the GL context, then select it
	Gtk::GLArea::on_realize();
	make_current();

	//Set up GLEW
	if(!m_isGlewInitialized)
	{
		//Check if GL was initialized OK
		if(has_error())
		{
			//doesn't seem to be any way to get this error without throwing it??
			try
			{
				throw_if_error();
			}
			catch(Glib::Error& gerr)
			{
				string err =
					"glscopeclient was unable to initialize OpenGL and cannot continue.\n"
					"This probably indicates a problem with your graphics card drivers.\n"
					"\n"
					"GL error: ";
				err += gerr.what();

				Gtk::MessageDialog dlg(
					err,
					false,
					Gtk::MESSAGE_ERROR,
					Gtk::BUTTONS_OK,
					true
					);

				dlg.run();
				exit(1);
			}
		}

		//Print some debug info
		auto context = get_context();
		if(!context)
			LogFatal("context is null but we don't have an error set in GTK\n");
		int major, minor;
		context->get_version(major, minor);
		string profile = "compatibility";
		if(context->is_legacy())
			profile = "legacy";
		else if(context->get_forward_compatible())
			profile = "core";
		string type = "";
		if(context->get_use_es())
			type = " ES";
		LogDebug("Context: OpenGL%s %d.%d %s profile\n",
			type.c_str(),
			major, minor,
			profile.c_str());
		{
			LogIndenter li;
			LogDebug("GL_VENDOR = %s\n", glGetString(GL_VENDOR));
			LogDebug("GL_RENDERER = %s\n", glGetString(GL_RENDERER));
			LogDebug("GL_VERSION = %s\n", glGetString(GL_VERSION));
			LogDebug("GL_SHADING_LANGUAGE_VERSION = %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
			LogDebug("Initial GL error code: %d\n", glGetError());
		}

		//Initialize GLEW
		GLenum glewResult = glewInit();
		if (glewResult != GLEW_OK)
		{
			string err =
				"glscopeclient was unable to initialize GLEW and cannot continue.\n"
				"This probably indicates a problem with your graphics card drivers.\n"
				"\n"
				"GLEW error: ";
			err += (const char*)glewGetErrorString(glewResult);

			Gtk::MessageDialog dlg(
				err,
				false,
				Gtk::MESSAGE_ERROR,
				Gtk::BUTTONS_OK,
				true
				);

			dlg.run();
			exit(1);
		}

		//Check for GL 4.2 (required for glBindImageTexture)
		if(!GLEW_VERSION_4_2)
		{
			string err =
				"Your graphics card or driver does not appear to support OpenGL 4.2.\n"
				"\n"
				"Unfortunately, glscopeclient cannot run on your system.\n";

			Gtk::MessageDialog dlg(
				err,
				false,
				Gtk::MESSAGE_ERROR,
				Gtk::BUTTONS_OK,
				true
				);

			dlg.run();
			exit(1);
		}

		//Make sure we have the required extensions
		if(	!GLEW_EXT_blend_equation_separate ||
			!GL_EXT_framebuffer_object ||
			!GL_ARB_vertex_array_object ||
			!GL_ARB_shader_storage_buffer_object ||
			!GL_ARB_compute_shader ||
			!GL_ARB_gpu_shader_int64 )
		{
			string err =
				"Your graphics card or driver does not appear to support one or more of the following required extensions:\n"
				"* GL_ARB_compute_shader\n"
				"* GL_ARB_gpu_shader_int64\n"
				"* GL_ARB_shader_storage_buffer_object\n"
				"* GL_ARB_vertex_array_object\n"
				"* GL_EXT_blend_equation_separate\n"
				"* GL_EXT_framebuffer_object\n"
				"\n"
				"Unfortunately, glscopeclient cannot run on your system.\n";

			Gtk::MessageDialog dlg(
				err,
				false,
				Gtk::MESSAGE_ERROR,
				Gtk::BUTTONS_OK,
				true
				);

			dlg.run();
			exit(1);
		}

		m_isGlewInitialized = true;
	}

	//We're about to draw the first frame after realization.
	//This means we need to save some configuration (like the current FBO) that GTK doesn't tell us directly
	m_firstFrame = true;

	//Create waveform render data for our main trace
	m_waveformRenderData = new WaveformRenderData(m_channel, this);

	//Set stuff up for each rendering pass
	InitializeWaveformPass();
	InitializeColormapPass();
	InitializePersistencePass();
	InitializeCairoPass();
	InitializeEyePass();
}

void WaveformArea::on_unrealize()
{
	make_current();

	CleanupGLHandles();

	Gtk::GLArea::on_unrealize();
}

void WaveformArea::CleanupGLHandles()
{
	//Clean up old shaders
	m_digitalWaveformComputeProgram.Destroy();
	m_analogWaveformComputeProgram.Destroy();
	m_colormapProgram.Destroy();
	m_persistProgram.Destroy();
	m_eyeProgram.Destroy();
	m_cairoProgram.Destroy();

	//Clean up old VAOs
	m_colormapVAO.Destroy();
	m_persistVAO.Destroy();
	m_cairoVAO.Destroy();
	m_eyeVAO.Destroy();

	//Clean up old VBOs
	m_colormapVBO.Destroy();
	m_persistVBO.Destroy();
	m_cairoVBO.Destroy();
	m_eyeVBO.Destroy();

	//Clean up old textures
	m_cairoTexture.Destroy();
	m_cairoTextureOver.Destroy();
	for(auto& e : m_eyeColorRamp)
		e.Destroy();

	delete m_waveformRenderData;
	m_waveformRenderData = NULL;
	for(auto it : m_overlayRenderData)
		delete it.second;
	m_overlayRenderData.clear();

	//Detach the FBO so we don't destroy it!!
	//GTK manages this, and it might be used by more than one waveform area within the application.
	m_windowFramebuffer.Detach();
}

void WaveformArea::InitializeWaveformPass()
{
	ComputeShader dwc;
	if(!dwc.Load("shaders/waveform-compute-digital.glsl"))
		LogFatal("failed to load digital waveform compute shader, aborting\n");
	m_digitalWaveformComputeProgram.Add(dwc);
	if(!m_digitalWaveformComputeProgram.Link())
		LogFatal("failed to link digital waveform shader program, aborting\n");

	ComputeShader awc;
	if(!awc.Load("shaders/waveform-compute-analog.glsl"))
		LogFatal("failed to load analog waveform compute shader, aborting\n");
	m_analogWaveformComputeProgram.Add(awc);
	if(!m_analogWaveformComputeProgram.Link())
		LogFatal("failed to link analog waveform shader program, aborting\n");
}

void WaveformArea::InitializeColormapPass()
{
	//Set up shaders
	VertexShader cvs;
	FragmentShader cfs;
	if(!cvs.Load("shaders/colormap-vertex.glsl") || !cfs.Load("shaders/colormap-fragment.glsl"))
		LogFatal("failed to load colormap shaders, aborting\n");

	m_colormapProgram.Add(cvs);
	m_colormapProgram.Add(cfs);
	if(!m_colormapProgram.Link())
		LogFatal("failed to link shader program, aborting\n");

	//Create the VAO/VBO for a fullscreen polygon
	float verts[8] =
	{
		-1, -1,
		 1, -1,
		 1,  1,
		-1,  1
	};
	m_colormapVBO.Bind();
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

	m_colormapVAO.Bind();
	m_colormapProgram.EnableVertexArray("vert");
	m_colormapProgram.SetVertexAttribPointer("vert", 2, 0);
}

void WaveformArea::InitializeEyePass()
{
	//Set up shaders
	VertexShader cvs;
	FragmentShader cfs;
	if(!cvs.Load("shaders/eye-vertex.glsl") || !cfs.Load("shaders/eye-fragment.glsl"))
		LogFatal("failed to load eye shaders, aborting\n");

	m_eyeProgram.Add(cvs);
	m_eyeProgram.Add(cfs);
	if(!m_eyeProgram.Link())
		LogFatal("failed to link shader program, aborting\n");

	//Create the VAO/VBO for a fullscreen polygon
	float verts[8] =
	{
		-1, -1,
		 1, -1,
		 1,  1,
		-1,  1
	};
	m_eyeVBO.Bind();
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

	m_eyeVAO.Bind();
	m_eyeProgram.EnableVertexArray("vert");
	m_eyeProgram.SetVertexAttribPointer("vert", 2, 0);

	//Load the eye color ramp
	char tmp[1024];
	const char* fnames[OscilloscopeWindow::NUM_EYE_COLORS];
	fnames[OscilloscopeWindow::EYE_CRT] = "gradients/eye-gradient-crt.rgba";
	fnames[OscilloscopeWindow::EYE_IRONBOW] = "gradients/eye-gradient-ironbow.rgba";
	fnames[OscilloscopeWindow::EYE_KRAIN] = "gradients/eye-gradient-krain.rgba";
	fnames[OscilloscopeWindow::EYE_RAINBOW] = "gradients/eye-gradient-rainbow.rgba";
	fnames[OscilloscopeWindow::EYE_GRAYSCALE] = "gradients/eye-gradient-grayscale.rgba";
	fnames[OscilloscopeWindow::EYE_VIRIDIS] = "gradients/eye-gradient-viridis.rgba";
	for(int i=0; i<OscilloscopeWindow::NUM_EYE_COLORS; i++)
	{
		FILE* fp = fopen(fnames[i], "r");
		if(!fp)
			LogFatal("fail to open eye gradient");
		fread(tmp, 1, 1024, fp);
		fclose(fp);

		m_eyeColorRamp[i].Bind();
		ResetTextureFiltering();
		m_eyeColorRamp[i].SetData(256, 1, tmp, GL_RGBA);
	}
}

void WaveformArea::InitializePersistencePass()
{
	//Set up shaders
	VertexShader cvs;
	FragmentShader cfs;
	if(!cvs.Load("shaders/persist-vertex.glsl") || !cfs.Load("shaders/persist-fragment.glsl"))
		LogFatal("failed to load persist shaders, aborting\n");

	m_persistProgram.Add(cvs);
	m_persistProgram.Add(cfs);
	if(!m_persistProgram.Link())
		LogFatal("failed to link shader program, aborting\n");

	//Create the VAO/VBO for a fullscreen polygon
	float verts[8] =
	{
		-1, -1,
		 1, -1,
		 1,  1,
		-1,  1
	};
	m_persistVBO.Bind();
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

	m_persistVAO.Bind();
	m_persistProgram.EnableVertexArray("vert");
	m_persistProgram.SetVertexAttribPointer("vert", 2, 0);
}

void WaveformArea::InitializeCairoPass()
{
	//Set up shaders
	VertexShader cvs;
	FragmentShader cfs;
	if(!cvs.Load("shaders/cairo-vertex.glsl") || !cfs.Load("shaders/cairo-fragment.glsl"))
		LogFatal("failed to load cairo shaders, aborting\n");

	m_cairoProgram.Add(cvs);
	m_cairoProgram.Add(cfs);
	if(!m_cairoProgram.Link())
		LogFatal("failed to link shader program, aborting\n");

	//Create the VAO/VBO for a fullscreen polygon
	float verts[8] =
	{
		-1, -1,
		 1, -1,
		 1,  1,
		-1,  1
	};
	m_cairoVBO.Bind();
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

	m_cairoVAO.Bind();
	m_cairoProgram.EnableVertexArray("vert");
	m_cairoProgram.SetVertexAttribPointer("vert", 2, 0);
}

bool WaveformArea::IsWaterfall()
{
	auto fall = dynamic_cast<Waterfall*>(m_channel.m_channel);
	return (fall != NULL);
}

bool WaveformArea::IsDigital()
{
	return (m_channel.m_channel->GetType() == OscilloscopeChannel::CHANNEL_TYPE_DIGITAL);
}

bool WaveformArea::IsAnalog()
{
	return (m_channel.m_channel->GetType() == OscilloscopeChannel::CHANNEL_TYPE_ANALOG);
}

bool WaveformArea::IsEye()
{
	return (m_channel.m_channel->GetType() == OscilloscopeChannel::CHANNEL_TYPE_EYE);
}

bool WaveformArea::IsEyeOrBathtub()
{
	//TODO: this should really be "is fixed two UI wide plot"
	auto bath = dynamic_cast<HorizontalBathtub*>(m_channel.m_channel);
	return IsEye() || (bath != NULL);
}

bool WaveformArea::IsTime()
{
	return (m_channel.m_channel->GetYAxisUnits().GetType() == Unit::UNIT_PS);
}
