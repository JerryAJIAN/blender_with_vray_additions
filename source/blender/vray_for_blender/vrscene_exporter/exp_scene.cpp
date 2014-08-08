/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Andrei Izrantcev <andrei.izrantcev@chaosgroup.com>
 *
 * * ***** END GPL LICENSE BLOCK *****
 */

#include "CGR_config.h"

#include "utils/CGR_vrscene.h"
#include "utils/CGR_string.h"
#include "utils/CGR_blender_data.h"
#include "utils/CGR_json_plugins.h"
#include "utils/CGR_rna.h"

#include "exp_scene.h"
#include "exp_nodes.h"

#include "GeomMayaHair.h"
#include "GeomStaticMesh.h"
#include "Light.h"

#include "PIL_time.h"
#include "BLI_string.h"
#include "BKE_material.h"
#include "BKE_global.h"
#include "BLI_math_matrix.h"

extern "C" {
#  include "RE_engine.h"
#  include "DNA_particle_types.h"
#  include "DNA_modifier_types.h"
#  include "DNA_material_types.h"
#  include "DNA_lamp_types.h"
#  include "DNA_camera_types.h"
#  include "BKE_anim.h"
#  include "DNA_windowmanager_types.h"
}

#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string/join.hpp>


// Default velocity transform matrix hex
const char* MyParticle::velocity = "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";

BL::Material  VRsceneExporter::m_mtlOverride = BL::Material(PointerRNA_NULL);


static int IsDuplicatorRenderable(BL::Object ob)
{
	if(NOT(ob.is_duplicator()))
		return true;

	if(ob.dupli_type() != BL::Object::dupli_type_NONE)
		return false;

	if(ob.particle_systems.length()) {
		BL::Object::particle_systems_iterator psysIt;
		for(ob.particle_systems.begin(psysIt); psysIt != ob.particle_systems.end(); ++psysIt) {
			BL::ParticleSystem   psys = *psysIt;
			BL::ParticleSettings pset = psys.settings();
			if(pset.use_render_emitter())
				return true;
		}
	}

	return false;
}


VRsceneExporter::VRsceneExporter()
{
	PRINT_INFO("VRsceneExporter::VRsceneExporter()");

	init();
}


VRsceneExporter::~VRsceneExporter()
{
	PRINT_INFO("VRsceneExporter::~VRsceneExporter()");

	m_skipObjects.clear();
}


void VRsceneExporter::addSkipObject(void *obPtr)
{
	m_skipObjects.insert(obPtr);
}


void VRsceneExporter::addToHideFromViewList(const std::string &listKey, void *obPtr)
{
	PRINT_INFO("Adding object '%s' to hide list '%s'...",
			   ((ID*)obPtr)->name, listKey.c_str());

	if(listKey == "all")
		m_hideFromView.visibility.insert(obPtr);
	else if (listKey == "camera")
		m_hideFromView.camera_visibility.insert(obPtr);
	else if (listKey == "gi")
		m_hideFromView.gi_visibility.insert(obPtr);
	else if (listKey == "reflect")
		m_hideFromView.reflections_visibility.insert(obPtr);
	else if (listKey == "refract")
		m_hideFromView.refractions_visibility.insert(obPtr);
	else if (listKey == "shadows")
		m_hideFromView.shadows_visibility.insert(obPtr);
}


void VRsceneExporter::init()
{
	VRayExportable::clearCache();

	ExpoterSettings::gSet.m_mtlOverride.clear();
	RnaAccess::RnaValue rna((ID*)ExpoterSettings::gSet.m_sce, "vray.SettingsOptions");
	if(rna.getBool("mtl_override_on")) {
		std::string overrideName = rna.getString("mtl_override");

		if(NOT(overrideName.empty())) {
			BL::BlendData::materials_iterator bl_maIt;
			for(ExpoterSettings::gSet.b_data.materials.begin(bl_maIt); bl_maIt != ExpoterSettings::gSet.b_data.materials.end(); ++bl_maIt) {
				BL::Material bl_ma = *bl_maIt;
				if(bl_ma.name() == overrideName) {
					ExpoterSettings::gSet.m_mtlOverride = Node::GetMaterialName((Material*)bl_ma.ptr.data);
					m_mtlOverride = bl_ma;
					break;
				}
			}
		}
	}

	std::string exporterRnaProperty = "vray.Exporter";

	RnaAccess::RnaValue vrayExporter((ID*)ExpoterSettings::gSet.m_sce, exporterRnaProperty.c_str());
	ExpoterSettings::gSet.m_useDisplaceSubdiv    = vrayExporter.getBool("use_displace");
	ExpoterSettings::gSet.m_useInstancerForGroup = vrayExporter.getBool("instancer_dupli_group");

	// Prepass LightLinker
	m_lightLinker.init(ExpoterSettings::gSet.b_data, ExpoterSettings::gSet.b_scene);
	m_lightLinker.prepass();
	m_lightLinker.setSceneSet(&m_exportedObjects);
	Node::m_lightLinker = &m_lightLinker;
	Node::m_scene_nodes = &m_exportedObjects;

	// Check what layers to use
	//
	int useLayers = vrayExporter.getEnum("activeLayers");

	// Current active layers
	if(useLayers == 0) {
		ExpoterSettings::gSet.m_activeLayers = ExpoterSettings::gSet.m_sce->lay;
	}
	// All layers
	else if(useLayers == 1) {
		ExpoterSettings::gSet.m_activeLayers = ~(1<<21);
	}
	// Custom layers
	else {
		// Load custom render layers
		int layer_values[20];
		RNA_boolean_get_array(vrayExporter.getPtr(), "customRenderLayers", layer_values);

		ExpoterSettings::gSet.m_activeLayers = 0;
		for(int a = 0; a < 20; ++a) {
			if(layer_values[a]) {
				ExpoterSettings::gSet.m_activeLayers |= (1 << a);
			}
		}
	}

	// Find if we need hide from view here
	int animationMode = vrayExporter.getEnum("animation_mode");

	// If "Camera Loop"
	if(animationMode == 4) {
		for(Camera *ca = (Camera*)ExpoterSettings::gSet.m_main->camera.first; ca; ca = (Camera*)ca->id.next) {
			RnaAccess::RnaValue vrayCamera((ID*)ca, "vray");
			int useHideFromView = vrayCamera.getBool("hide_from_view");
			if(useHideFromView) {
				ExpoterSettings::gSet.m_useHideFromView = true;
				break;
			}
		}
	}
	else {
		RnaAccess::RnaValue vrayCamera((ID*)ExpoterSettings::gSet.m_sce->camera->data, "vray");
		ExpoterSettings::gSet.m_useHideFromView = vrayCamera.getBool("hide_from_view");
	}
}


int VRsceneExporter::exportScene(const int &exportNodes, const int &exportGeometry)
{
	PRINT_INFO("VRsceneExporter::exportScene()");

	ExpoterSettings::gSet.m_exportNodes  = exportNodes;
	ExpoterSettings::gSet.m_exportMeshes = exportGeometry;

	double timeMeasure = 0.0;
	char   timeMeasureBuf[32];

	PRINT_INFO_EX("Exporting data for frame %i...", ExpoterSettings::gSet.m_frameCurrent);
	timeMeasure = PIL_check_seconds_timer();

	Base *base = NULL;

	ExpoterSettings::gSet.b_engine.update_progress(0.0f);

	PointerRNA sceneRNA;
	RNA_id_pointer_create((ID*)ExpoterSettings::gSet.m_sce, &sceneRNA);
	BL::Scene bl_sce(sceneRNA);

	size_t nObjects = bl_sce.objects.length();

	float  expProgress = 0.0f;
	float  expProgStep = 1.0f / nObjects;
	int    progUpdateCnt = nObjects > 3000 ? 1000 : 100;
	if(nObjects > 3000) {
		progUpdateCnt = 1000;
	}
	else if(nObjects < 200) {
		progUpdateCnt = 10;
	}
	else {
		progUpdateCnt = 100;
	}

	// Clear caches
	m_exportedObjects.clear();
	m_psys.clear();

	// Create particle system data
	// Needed for the correct first frame
	//
	if(ExpoterSettings::gSet.m_isAnimation) {
		if(ExpoterSettings::gSet.m_frameCurrent == ExpoterSettings::gSet.m_frameStart) {
			initDupli();
		}
	}

	VRayNodeContext nodeCtx;
	VRayNodeExporter::exportVRayEnvironment(&nodeCtx);

	// Export stuff
	int exportInterrupt = false;

	base = (Base*)ExpoterSettings::gSet.m_sce->base.first;
	nObjects = 0;
	while(base) {
		if(ExpoterSettings::gSet.b_engine.test_break()) {
			ExpoterSettings::gSet.b_engine.report(RPT_WARNING, "Export interrupted!");
			exportInterrupt = true;
			break;
		}

		Object *ob = base->object;
		base = base->next;

		// PRINT_INFO("Processing '%s'...", ob->id.name);

		// Skip object here, but not in dupli!
		// Dupli could be particles and it's better to
		// have animated 'visible' param there
		//
		if(ob->restrictflag & OB_RESTRICT_RENDER)
			continue;

		if(NOT(ob->lay & ExpoterSettings::gSet.m_activeLayers))
			continue;

		if(m_skipObjects.count((void*)&ob->id)) {
			PRINT_INFO("Skipping object: %s", ob->id.name);
			continue;
		}

		exportObjectBase(ob);

		expProgress += expProgStep;
		nObjects++;
		if((nObjects % progUpdateCnt) == 0) {
			ExpoterSettings::gSet.b_engine.update_progress(expProgress);
		}
	}

	if(NOT(exportInterrupt)) {
		// Export dupli/particle systems
		//
		exportDupli();

		// Export materials
		//
		BL::BlendData b_data(PointerRNA_NULL);
		BL::BlendData::materials_iterator maIt;
		
		if (ExpoterSettings::gSet.b_engine.is_preview()) {
			RenderEngine *re = (RenderEngine*)ExpoterSettings::gSet.b_engine.ptr.data;
			if(re->type->preview_main) {
				PointerRNA previewMainPtr;
				RNA_id_pointer_create((ID*)re->type->preview_main, &previewMainPtr);
				b_data = BL::BlendData(previewMainPtr);
			}
		}

		if(NOT(b_data))
			b_data = ExpoterSettings::gSet.b_data;

		if(m_mtlOverride)
			VRayNodeExporter::exportMaterial(b_data, m_mtlOverride);

		for(b_data.materials.begin(maIt); maIt != b_data.materials.end(); ++maIt) {
			BL::Material b_ma = *maIt;
			if(m_mtlOverride && Node::DoOverrideMaterial(b_ma))
				continue;
			VRayNodeExporter::exportMaterial(b_data, b_ma);
		}
	}

	m_lightLinker.write(ExpoterSettings::gSet.m_fileObject);

	ExpoterSettings::gSet.b_engine.update_progress(1.0f);

	m_hideFromView.clear();

	BLI_timestr(PIL_check_seconds_timer()-timeMeasure, timeMeasureBuf, sizeof(timeMeasureBuf));

	if(exportInterrupt) {
		PRINT_INFO_EX("Exporting data for frame %i is interruped! [%s]",
					  ExpoterSettings::gSet.m_frameCurrent, timeMeasureBuf);
		return 1;
	}

	PRINT_INFO_EX("Exporting data for frame %i done [%s]",
				  ExpoterSettings::gSet.m_frameCurrent, timeMeasureBuf);

	return 0;
}


void VRsceneExporter::exportObjectBase(Object *ob)
{
	if(NOT(GEOM_TYPE(ob) || EMPTY_TYPE(ob) || LIGHT_TYPE(ob)))
		return;

	PointerRNA objectRNA;
	RNA_id_pointer_create((ID*)ob, &objectRNA);
	BL::Object bl_ob(objectRNA);

	PRINT_INFO("Processing object %s", ob->id.name);

	if(ob->id.pad2)
		PRINT_INFO("Base object %s (update: %i)", ob->id.name, ob->id.pad2);

	if(bl_ob.is_duplicator()) {
		// If object is a dupli group holder and it's not animated -
		// export it only for the first frame
		//
		if(ExpoterSettings::gSet.DoUpdateCheck()) {
			if(bl_ob.dupli_type() == BL::Object::dupli_type_GROUP) {
				if(NOT(VRayScene::Node::IsUpdated((Object*)bl_ob.ptr.data))) {
					return;
				}
			}
		}

		bl_ob.dupli_list_create(ExpoterSettings::gSet.b_scene, 2);

		RnaAccess::RnaValue bl_obRNA((ID*)ob, "vray");
		int override_objectID = bl_obRNA.getInt("dupliGroupIDOverride");

		int useInstancer = true;
		if(bl_ob.dupli_type() == BL::Object::dupli_type_GROUP) {
			useInstancer = ExpoterSettings::gSet.m_useInstancerForGroup;
		}
		else {
			PointerRNA vrayObject = RNA_pointer_get(&bl_ob.ptr, "vray");

			useInstancer = RNA_boolean_get(&vrayObject, "use_instancer");
		}

		NodeAttrs dupliAttrs;
		dupliAttrs.override = true;
		// If dupli are shown via Instancer we need to hide
		// original object
		dupliAttrs.visible  = NOT(useInstancer);
		dupliAttrs.objectID = override_objectID;
		dupliAttrs.dupliHolder = bl_ob;

		const std::string &duplicatorName = GetIDName(bl_ob);

		BL::Object::dupli_list_iterator b_dup;
		for(bl_ob.dupli_list.begin(b_dup); b_dup != bl_ob.dupli_list.end(); ++b_dup) {
			if(ExpoterSettings::gSet.b_engine.test_break())
				break;

			BL::DupliObject bl_dupliOb      = *b_dup;
			BL::Object      bl_duplicatedOb =  bl_dupliOb.object();

			if(bl_dupliOb.hide() || bl_duplicatedOb.hide_render())
				continue;

			if(NOT(IsDuplicatorRenderable(bl_duplicatedOb)))
				continue;

			DupliObject *dupliOb = (DupliObject*)bl_dupliOb.ptr.data;

			if(NOT(GEOM_TYPE(dupliOb->ob) || LIGHT_TYPE(dupliOb->ob)))
				continue;

			if(bl_duplicatedOb.type() == BL::Object::type_LAMP) {
#if CGR_EXPORT_LIGHTS_CPP
				exportLightNoded(ob, dupliOb);
#else
				exportLight(ob, dupliOb);
#endif
			}
			else {
				if(NOT(useInstancer)) {
					dupliAttrs.namePrefix = bl_ob.name() + "@" + BOOST_FORMAT_INT(dupliOb->persistent_id[0]);
					dupliAttrs.namePrefix = StripString(dupliAttrs.namePrefix);
					copy_m4_m4(dupliAttrs.tm, dupliOb->mat);

					// If LightLinker contain duplicator,
					// we need to exclude it's objects
					//
					std::string pluginName = dupliAttrs.namePrefix + GetIDName((ID*)dupliOb->ob);
					m_lightLinker.excludePlugin(duplicatorName, pluginName);

					exportObject(dupliOb->ob, true, dupliAttrs);
				}
				else {
					std::string dupliBaseName;

					BL::ParticleSystem bl_psys = bl_dupliOb.particle_system();
					if(NOT(bl_psys))
						dupliBaseName = bl_ob.name();
					else {
						BL::ParticleSettings bl_pset = bl_psys.settings();
						dupliBaseName = bl_ob.name() + bl_psys.name() + bl_pset.name();
					}

					MyPartSystem *mySys = m_psys.get(dupliBaseName);
					MyParticle *myPa = new MyParticle();
					myPa->nodeName = GetIDName(&dupliOb->ob->id);
					myPa->particleId = dupliOb->persistent_id[0];
					// Instancer use original object's transform,
					// so apply inverse matrix here.
					// When linking from file 'imat' is not valid,
					// so better to always calculate inverse matrix ourselves.
					//
					float duplicatedTmInv[4][4];
					copy_m4_m4(duplicatedTmInv, dupliOb->ob->obmat);
					invert_m4(duplicatedTmInv);
					float dupliTm[4][4];
					mul_m4_m4m4(dupliTm, dupliOb->mat, duplicatedTmInv);
					GetTransformHex(dupliTm, myPa->transform);

					mySys->append(myPa);

					// Set original object transform
					copy_m4_m4(dupliAttrs.tm, dupliOb->ob->obmat);
					exportObject(dupliOb->ob, false, dupliAttrs);
				}
			}
		}

		bl_ob.dupli_list_clear();

		if(ob->transflag & OB_DUPLI) {
			// If dupli were not from particles (eg DupliGroup) skip base object
			if(NOT(ob->transflag & OB_DUPLIPARTS))
				return;
			else {
				// If there is fur we will check for "Render Emitter" later
				if(NOT(Node::HasHair(ob)))
					if(NOT(Node::DoRenderEmitter(ob)))
						return;
			}
		}
	}

	if(ExpoterSettings::gSet.b_engine.test_break())
		return;

	if(GEOM_TYPE(ob)) {
		// Smoke domain will be exported from Effects
		if(Node::IsSmokeDomain(ob))
			return;
		exportObject(ob);
	}
	else if(LIGHT_TYPE(ob)) {
#if CGR_EXPORT_LIGHTS_CPP
		exportLightNoded(ob);
#endif
	}
}


void VRsceneExporter::exportObject(Object *ob, const int &checkUpdated, const NodeAttrs &attrs)
{
	const std::string idName = attrs.namePrefix + GetIDName(&ob->id);

	if(m_exportedObjects.count(idName))
		return;
	m_exportedObjects.insert(idName);

	PointerRNA objectRNA;
	RNA_id_pointer_create((ID*)ob, &objectRNA);
	BL::Object bl_ob(objectRNA);

	PointerRNA vrayObject  = RNA_pointer_get(&bl_ob.ptr, "vray");
	PointerRNA vrayClipper = RNA_pointer_get(&vrayObject, "VRayClipper");

	if(RNA_boolean_get(&vrayClipper, "enabled")) {
		VRayNodeExporter::exportVRayClipper(ExpoterSettings::gSet.b_data, bl_ob);
	}
	else {
		BL::NodeTree ntree = VRayNodeExporter::getNodeTree(ExpoterSettings::gSet.b_data, (ID*)ob);
		if(ntree) {
			exportNodeFromNodeTree(ntree, ob, attrs);
		}
		else {
			exportNode(ob, checkUpdated, attrs);
		}
	}
}


void VRsceneExporter::exportNode(Object *ob, const int &checkUpdated, const NodeAttrs &attrs)
{
	PRINT_INFO("VRsceneExporter::exportNode(%s)",
			   ob->id.name);

	Node *node = new Node(ExpoterSettings::gSet.m_sce, ExpoterSettings::gSet.m_main, ob);
	node->setNamePrefix(attrs.namePrefix);
	if(attrs.override) {
		NodeAttrs &_attrs = const_cast<NodeAttrs&>(attrs);
		node->setTransform(_attrs.tm);
		node->setVisiblity(attrs.visible);
		if(attrs.objectID > -1) {
			node->setObjectID(attrs.objectID);
		}
		if(attrs.dupliHolder.ptr.data) {
			node->setDupliHolder(attrs.dupliHolder);
		}
	}
	node->init(ExpoterSettings::gSet.m_mtlOverride);
	node->initHash();

	if(ExpoterSettings::gSet.m_useHideFromView && m_hideFromView.hasData()) {
		RenderStats hideFromViewStats;
		hideFromViewStats.visibility             = !m_hideFromView.visibility.count(ob);
		hideFromViewStats.gi_visibility          = !m_hideFromView.gi_visibility.count(ob);

		hideFromViewStats.reflections_visibility = !m_hideFromView.reflections_visibility.count(ob);
		hideFromViewStats.refractions_visibility = !m_hideFromView.refractions_visibility.count(ob);
		hideFromViewStats.shadows_visibility     = !m_hideFromView.shadows_visibility.count(ob);
		hideFromViewStats.camera_visibility      = !m_hideFromView.camera_visibility.count(ob);

		node->setHideFromView(hideFromViewStats);
	}

	// This will also check if object's mesh is valid
	if(NOT(node->preInitGeometry(ExpoterSettings::gSet.m_useDisplaceSubdiv))) {
		delete node;
		return;
	}

	if(node->hasHair()) {
		node->writeHair();
		if(NOT(node->doRenderEmitter())) {
			delete node;
			return;
		}
	}

	if(ExpoterSettings::gSet.m_exportMeshes) {
		int writeData = true;
		if(checkUpdated && ExpoterSettings::gSet.DoUpdateCheck())
			writeData = node->isObjectDataUpdated();
		if(writeData) {
			node->initGeometry();
			node->writeGeometry(ExpoterSettings::gSet.m_fileGeom, ExpoterSettings::gSet.m_frameCurrent);
		}
	}

	if(ExpoterSettings::gSet.m_exportNodes && NOT(node->isMeshLight())) {
		int writeObject = true;
		if(checkUpdated && ExpoterSettings::gSet.DoUpdateCheck())
			writeObject = node->isObjectUpdated();
		int toDelete = false;
		if(writeObject) {
			toDelete = node->write(ExpoterSettings::gSet.m_fileObject, ExpoterSettings::gSet.m_frameCurrent);
		}
		else {
			if(m_hideFromView.hasData()) {
				node->writeHideFromView();
			}
		}
		if(toDelete) {
			delete node;
		}
	}
	else {
		delete node;
	}
}


void VRsceneExporter::exportNodeFromNodeTree(BL::NodeTree ntree, Object *ob, const NodeAttrs &attrs)
{
	PRINT_INFO("VRsceneExporter::exportNodeFromNodeTree(%s)",
			   ob->id.name);

	PointerRNA objectRNA;
	RNA_id_pointer_create((ID*)ob, &objectRNA);
	BL::Object bl_ob(objectRNA);

	// Export hair
	//
	Node::WriteHair(ob, attrs);

	if(NOT(Node::DoRenderEmitter(ob)))
		return;

	// Export object itself
	//
	BL::Node nodeOutput = VRayNodeExporter::getNodeByType(ntree, "VRayNodeObjectOutput");
	if(NOT(nodeOutput)) {
		PRINT_ERROR("Object: %s Node tree: %s => Output node not found!",
					ob->id.name, ntree.name().c_str());
		return;
	}

	BL::NodeSocket geometrySocket = VRayNodeExporter::getSocketByName(nodeOutput, "Geometry");
	if(NOT(geometrySocket && geometrySocket.is_linked())) {
		PRINT_ERROR("Object: %s Node tree: %s => Geometry node is not set!",
					ob->id.name, ntree.name().c_str());
		return;
	}

	const std::string pluginName = attrs.namePrefix + GetIDName((ID*)ob);

	char transform[CGR_TRANSFORM_HEX_SIZE];
	GetTransformHex(ob->obmat, transform);

	int visible  = true;
	int objectID = ob->index;

	// Prepare object context
	//
	VRayNodeContext nodeCtx;
	nodeCtx.obCtx.ob   = ob;
	nodeCtx.obCtx.sce  = ExpoterSettings::gSet.m_sce;
	nodeCtx.obCtx.main = ExpoterSettings::gSet.m_main;
	nodeCtx.obCtx.mtlOverride = ExpoterSettings::gSet.m_mtlOverride;

	// Export object main properties
	//
	std::string geometry = VRayNodeExporter::exportSocket(ntree, geometrySocket, &nodeCtx);
	if(geometry == "NULL") {
		PRINT_ERROR("Object: %s Node tree: %s => Incorrect geometry!",
					ob->id.name, ntree.name().c_str());
		return;
	}

	BL::Node geometryNode = VRayNodeExporter::getConnectedNode(geometrySocket, &nodeCtx);
	if(geometryNode.bl_idname() == "VRayNodeLightMesh") {
		// No need to export Node - this object is LightMesh
		return;
	}

	BL::NodeSocket materialSocket = VRayNodeExporter::getSocketByName(nodeOutput, "Material");
	if(NOT(materialSocket && materialSocket.is_linked())) {
		PRINT_ERROR("Object: %s Node tree: %s => Material node is not set!",
					ob->id.name, ntree.name().c_str());
		return;
	}

	std::string material = VRayNodeExporter::exportSocket(ntree, materialSocket, &nodeCtx);
	if(material == "NULL") {
		PRINT_ERROR("Object: %s Node tree: %s => Incorrect material!",
					ob->id.name, ntree.name().c_str());
		return;
	}

	// Add MtlRenderStats and MtlWrapper from Object level for "one click" things
	//
	PointerRNA vrayObject = RNA_pointer_get(&bl_ob.ptr, "vray");

	material = Node::WriteMtlWrapper(&vrayObject, NULL, pluginName, material);
	material = Node::WriteMtlRenderStats(&vrayObject, NULL, pluginName, material);

	// Export 'MtlRenderStats' for "Hide From View"
	//
	if(ExpoterSettings::gSet.m_useHideFromView && m_hideFromView.hasData()) {
		std::string hideFromViewName = "HideFromView@" + pluginName;

		AttributeValueMap hideFromViewAttrs;
		hideFromViewAttrs["base_mtl"] = material;
		hideFromViewAttrs["visibility"]             = BOOST_FORMAT_BOOL(!m_hideFromView.visibility.count(ob));
		hideFromViewAttrs["gi_visibility"]          = BOOST_FORMAT_BOOL(!m_hideFromView.gi_visibility.count(ob));
		hideFromViewAttrs["camera_visibility"]      = BOOST_FORMAT_BOOL(!m_hideFromView.camera_visibility.count(ob));
		hideFromViewAttrs["reflections_visibility"] = BOOST_FORMAT_BOOL(!m_hideFromView.reflections_visibility.count(ob));
		hideFromViewAttrs["refractions_visibility"] = BOOST_FORMAT_BOOL(!m_hideFromView.refractions_visibility.count(ob));
		hideFromViewAttrs["shadows_visibility"]     = BOOST_FORMAT_BOOL(!m_hideFromView.shadows_visibility.count(ob));

		// It's actually a material, but we will write it along with Node
		VRayNodePluginExporter::exportPlugin("NODE", "MtlRenderStats", hideFromViewName, hideFromViewAttrs);

		material = hideFromViewName;
	}

	// Check if we need to override some stuff;
	// comes from advanced DupliGroup export.
	//
	if(attrs.override) {
		std::string overrideBaseName = pluginName + "@" + GetIDName((ID*)attrs.dupliHolder.ptr.data);

		PointerRNA vrayObject = RNA_pointer_get((PointerRNA*)&attrs.dupliHolder.ptr, "vray");

		visible  = attrs.visible;
		objectID = attrs.objectID;

		NodeAttrs &_attrs = const_cast<NodeAttrs&>(attrs);
		GetTransformHex(_attrs.tm, transform);

		material = Node::WriteMtlWrapper(&vrayObject, NULL, overrideBaseName, material);
		material = Node::WriteMtlRenderStats(&vrayObject, NULL, overrideBaseName, material);
	}

	PointerRNA vrayNode = RNA_pointer_get(&vrayObject, "Node");

	StrVector user_attributes;
	VRayNodeExporter::getUserAttributes(&vrayNode, user_attributes);

	AttributeValueMap pluginAttrs;
	pluginAttrs["material"]  = material;
	pluginAttrs["geometry"]  = geometry;
	pluginAttrs["objectID"]  = BOOST_FORMAT_INT(objectID);
	pluginAttrs["visible"]   = BOOST_FORMAT_INT(visible);
	pluginAttrs["transform"] = BOOST_FORMAT_TM(transform);

	if (user_attributes.size()) {
		pluginAttrs["user_attributes"] = BOOST_FORMAT_STRING(BOOST_FORMAT_LIST_JOIN_SEP(user_attributes, ";"));
	}

	VRayNodePluginExporter::exportPlugin("NODE", "Node", pluginName, pluginAttrs);
}


void VRsceneExporter::exportLight(Object *ob, DupliObject *dOb)
{
	if(NOT(ExpoterSettings::gSet.m_exportNodes))
		return;

	Light *light = new Light(ExpoterSettings::gSet.m_sce, ExpoterSettings::gSet.m_main, ob, dOb);

	int toDelete = light->write(ExpoterSettings::gSet.m_fileLights, ExpoterSettings::gSet.m_frameCurrent);
	if(toDelete) {
		delete light;
	}
}

void VRsceneExporter::exportLightNoded(Object *ob, DupliObject *dOb)
{
	// TODO: Export lights with nodes
	//
	Lamp *lamp = (Lamp*)ob->data;

	PointerRNA lampRNA;
	RNA_id_pointer_create((ID*)lamp, &lampRNA);

	PointerRNA vrayLamp = RNA_pointer_get(&lampRNA, "vray");

	std::string       pluginID;
	AttributeValueMap pluginAttrs;

	if(lamp->type == LA_AREA) {
		pluginID = "LightRectangle";

		float sizeX = lamp->area_size / 2.0f;
		float sizeY = lamp->area_shape == LA_AREA_SQUARE ? sizeX : lamp->area_sizey / 2.0f;

		pluginAttrs["u_size"] = BOOST_FORMAT_FLOAT(sizeX);
		pluginAttrs["v_size"] = BOOST_FORMAT_FLOAT(sizeY);
	}
	else if(lamp->type == LA_HEMI) {
		pluginID = "LightDome";
	}
	else if(lamp->type == LA_SPOT) {
		int spotType = RNA_enum_get(&vrayLamp, "spot_type");
		switch(spotType) {
			case 0: {
				pluginID = "LightSpotMax";

				pluginAttrs["fallsize"] = BOOST_FORMAT_FLOAT(lamp->spotsize);

				break;
			}
			case 1: pluginID = "LightIESMax";  break;
		}
	}
	else if(lamp->type == LA_LOCAL) {
		int omniType = RNA_enum_get(&vrayLamp, "omni_type");
		switch(omniType) {
			case 0: pluginID = "LightOmniMax";    break;
			case 1: pluginID = "LightAmbientMax"; break;
			case 2: pluginID = "LightSphere";     break;
		}
	}
	else if(lamp->type == LA_SUN) {
		int directType = RNA_enum_get(&vrayLamp, "direct_type");
		switch(directType) {
			case 0: pluginID = "LightDirectMax"; break;
			case 1: pluginID = "SunLight";       break;
		}
	}
	else {
		PRINT_ERROR("Lamp: %s Type: %i => Lamp type is not supported!",
					ob->id.name+2, lamp->type);
		return;
	}

	std::string pluginName = GetIDName((ID*)ob);

	PointerRNA        propGroup = RNA_pointer_get(&vrayLamp, pluginID.c_str());

	// Get all non-mappable attribute values
	StrSet pluginAttrNames;

	VRayNodeExporter::getAttributesList(pluginID, pluginAttrNames, false);

	for(StrSet::const_iterator setIt = pluginAttrNames.begin(); setIt != pluginAttrNames.end(); ++setIt) {
		std::string attrName = *setIt;

		std::string propValue = VRayNodeExporter::getValueFromPropGroup(&propGroup, (ID*)lamp, attrName.c_str());
		if(propValue != "NULL")
			pluginAttrs[attrName] = propValue;
	}

	// Now, get all mappable attribute values
	//
	BL::NodeTree lampNtree = VRayNodeExporter::getNodeTree(ExpoterSettings::gSet.b_data, (ID*)lamp);
	if(lampNtree) {
		std::string vrayNodeType = boost::str(boost::format("VRayNode%s") % pluginID);

		BL::Node lampNode = VRayNodeExporter::getNodeByType(lampNtree, vrayNodeType);
		if(lampNode) {
			StrSet socketAttrNames;

			VRayNodeExporter::getAttributesList(pluginID, socketAttrNames, true);

			for(StrSet::const_iterator setIt = socketAttrNames.begin(); setIt != socketAttrNames.end(); ++setIt) {
				std::string attrName = *setIt;

				BL::NodeSocket sock = VRayNodeExporter::getSocketByAttr(lampNode, attrName);
				if(sock) {
					std::string socketValue = VRayNodeExporter::exportSocket(lampNtree, sock);
					if(socketValue != "NULL")
						pluginAttrs[attrName] = socketValue;
				}
			}
		}
	}

	// Now, let's go through "Render Elements" and check if we have to
	// plug our light somewhere like "Light Select"
	//
	BL::NodeTree sceNtree = VRayNodeExporter::getNodeTree(ExpoterSettings::gSet.b_data,
														  (ID*)ExpoterSettings::gSet.m_sce);
	if(sceNtree) {
		BL::Node chanNode = VRayNodeExporter::getNodeByType(sceNtree, "VRayNodeRenderChannels");
		if(chanNode) {
			// TODO:
		}
	}

	char transform[CGR_TRANSFORM_HEX_SIZE];
	if(dOb)
		GetTransformHex(dOb->mat, transform);
	else
		GetTransformHex(ob->obmat, transform);

	pluginAttrs["transform"] = BOOST_FORMAT_TM(transform);

	VRayNodePluginExporter::exportPlugin("LIGHT", pluginID, pluginName, pluginAttrs);
}


void VRsceneExporter::initDupli()
{
	PointerRNA sceneRNA;
	RNA_id_pointer_create((ID*)ExpoterSettings::gSet.m_sce, &sceneRNA);
	BL::Scene bl_sce(sceneRNA);

	BL::Scene::objects_iterator bl_obIt;
	for(bl_sce.objects.begin(bl_obIt); bl_obIt != bl_sce.objects.end(); ++bl_obIt) {
		BL::Object bl_ob = *bl_obIt;
		if(bl_ob.type() == BL::Object::type_META)
			continue;
		if(bl_ob.is_duplicator()) {
			if(bl_ob.particle_systems.length()) {
				BL::Object::particle_systems_iterator bl_psysIt;
				for(bl_ob.particle_systems.begin(bl_psysIt); bl_psysIt != bl_ob.particle_systems.end(); ++bl_psysIt) {
					BL::ParticleSystem bl_psys = *bl_psysIt;
					BL::ParticleSettings bl_pset = bl_psys.settings();

					if(bl_pset.type() == BL::ParticleSettings::type_HAIR && bl_pset.render_type() == BL::ParticleSettings::render_type_PATH)
						continue;

					m_psys.get(bl_pset.name());
				}
			}
			if(bl_ob.dupli_type() != BL::Object::dupli_type_NONE)
				m_psys.get(bl_ob.name());
		}
	}
}


void VRsceneExporter::exportDupli()
{
	PyObject *out = ExpoterSettings::gSet.m_fileObject;

	for(MyPartSystems::const_iterator sysIt = m_psys.m_systems.begin(); sysIt != m_psys.m_systems.end(); ++sysIt) {
		const std::string   psysName = sysIt->first;
		const MyPartSystem *parts    = sysIt->second;

		PYTHON_PRINTF(out, "\nInstancer Dupli%s {", StripString(psysName).c_str());
		PYTHON_PRINTF(out, "\n\tinstances=%sList(%i", VRayExportable::m_interpStart, ExpoterSettings::gSet.m_isAnimation ? ExpoterSettings::gSet.m_frameCurrent : 0);
		if(parts->size()) {
			PYTHON_PRINT(out, ",");
			for(Particles::const_iterator paIt = parts->m_particles.begin(); paIt != parts->m_particles.end(); ++paIt) {
				const MyParticle *pa = *paIt;

				PYTHON_PRINTF(out, "List("SIZE_T_FORMAT",TransformHex(\"%s\"),TransformHex(\"%s\"),%s)", pa->particleId, pa->transform, pa->velocity, pa->nodeName.c_str());

				if(paIt != --parts->m_particles.end()) {
					PYTHON_PRINT(out, ",");
				}
			}
		}
		PYTHON_PRINTF(out, ")%s;", VRayExportable::m_interpEnd);
		PYTHON_PRINTF(ExpoterSettings::gSet.m_fileObject, "\n}\n");
	}

	m_psys.clear();
}
