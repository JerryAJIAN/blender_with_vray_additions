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

#include "vfb_node_exporter.h"
#include "vfb_utils_mesh.h"



AttrValue DataExporter::exportGeomStaticMesh(BL::Object ob)
{
	AttrValue geom;

	PluginDesc geomDesc(getMeshName(ob), "GeomStaticMesh", "Geom@");

	VRayForBlender::Mesh::ExportOptions options;
	options.merge_channel_vertices = false;

	int err = VRayForBlender::Mesh::FillMeshData(m_data, m_scene, ob, options, geomDesc);
	if (!err) {
		geom = m_exporter->export_plugin(geomDesc);
	}

	return geom;
}
