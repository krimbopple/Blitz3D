#include "std.h"
#include "loader_gltf.h"
#include "meshmodel.h"
#include "animation.h"
#include "pivot.h"

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION

#include "tinygltf/tiny_gltf.h"

extern gxRuntime* gx_runtime;
static map<string, MeshModel*> nodes_map;
static int anim_len;

static bool conv, flip_tris;
static Transform conv_tform;
static bool collapse, animonly;

struct NodeAnimData {
    map<int, Quat> rotationKeys;
    map<int, Vector> scaleKeys;
    map<int, Vector> positionKeys;
};
 // TODO: this animation stuff is totally fucked.
static void parseAnimations(const tinygltf::Model& model, map<int, NodeAnimData>& nodeAnims) {
    for (const auto& anim : model.animations) {
        for (const auto& channel : anim.channels) {
            int nodeIndex = channel.target_node;
            if (nodeIndex < 0 || nodeIndex >= model.nodes.size()) continue;

            const auto& sampler = anim.samplers[channel.sampler];

            if (nodeAnims.find(nodeIndex) == nodeAnims.end()) {
                nodeAnims[nodeIndex] = NodeAnimData();
            }
            NodeAnimData& animData = nodeAnims[nodeIndex];

            const auto& inputAccessor = model.accessors[sampler.input];
            const auto& outputAccessor = model.accessors[sampler.output];

            const auto& inputBufferView = model.bufferViews[inputAccessor.bufferView];
            const auto& outputBufferView = model.bufferViews[outputAccessor.bufferView];

            const auto& inputBuffer = model.buffers[inputBufferView.buffer];
            const auto& outputBuffer = model.buffers[outputBufferView.buffer];

            const float* times = reinterpret_cast<const float*>(
                &inputBuffer.data[inputBufferView.byteOffset + inputAccessor.byteOffset]);
            const float* values = reinterpret_cast<const float*>(
                &outputBuffer.data[outputBufferView.byteOffset + outputAccessor.byteOffset]);

            int numKeys = (int)inputAccessor.count;

            for (int k = 0; k < numKeys; ++k) {
                int time = static_cast<int>(times[k] * 1000);
                if (time > anim_len) anim_len = time;

                string target_path = channel.target_path;
                if (target_path == "rotation") {
                    if (outputAccessor.type == TINYGLTF_TYPE_VEC4) {
                        float x = values[k * 4];
                        float y = values[k * 4 + 1];
                        float z = values[k * 4 + 2];
                        float w = values[k * 4 + 3];

                        Quat rot(w, Vector(x, y, z));
                        if (conv) {
                            if (fabs(rot.w) < 1 - EPSILON) {
                                rot.normalize();
                                float half = acosf(rot.w);
                                if (flip_tris) half = -half;
                                Vector axis = rot.v.normalized();
                                rot = Quat(cosf(half), (conv_tform.m * axis).normalized() * sinf(half));
                            }
                            else {
                                rot = Quat(1, Vector(0, 0, 0));
                            }
                        }
                        animData.rotationKeys[time] = rot;
                    }
                }
                else if (target_path == "scale") {
                    if (outputAccessor.type == TINYGLTF_TYPE_VEC3) {
                        Vector scl(values[k * 3], values[k * 3 + 1], values[k * 3 + 2]);
                        if (conv) scl = conv_tform.m * scl;
                        scl.x = fabs(scl.x); scl.y = fabs(scl.y); scl.z = fabs(scl.z);
                        animData.scaleKeys[time] = scl;
                    }
                }
                else if (target_path == "translation") {
                    if (outputAccessor.type == TINYGLTF_TYPE_VEC3) {
                        Vector pos(values[k * 3], values[k * 3 + 1], values[k * 3 + 2]);
                        if (conv) pos = conv_tform * pos;
                        animData.positionKeys[time] = pos;
                    }
                }
            }
        }
    }
}

static void applyAnimationToNode(MeshModel* node, const NodeAnimData& animData) {
    if (animData.rotationKeys.empty() && animData.scaleKeys.empty() && animData.positionKeys.empty())
        return;

    Animation anim;

    for (const auto& kv : animData.rotationKeys) {
        anim.setRotationKey(kv.first, kv.second);
    }

    for (const auto& kv : animData.scaleKeys) {
        anim.setScaleKey(kv.first, kv.second);
    }

    for (const auto& kv : animData.positionKeys) {
        anim.setPositionKey(kv.first, kv.second);
    }

    node->setAnimation(anim);
}

static Brush parseMaterial(const tinygltf::Material& material) {
    Brush brush;

    if (material.pbrMetallicRoughness.baseColorFactor.size() >= 3) {
        Vector color(
            (float)material.pbrMetallicRoughness.baseColorFactor[0],
            (float)material.pbrMetallicRoughness.baseColorFactor[1],
            (float)material.pbrMetallicRoughness.baseColorFactor[2]
        );
        brush.setColor(color);

        if (material.pbrMetallicRoughness.baseColorFactor.size() >= 4) {
            brush.setAlpha((float)material.pbrMetallicRoughness.baseColorFactor[3]);
        }
    }

    // we can just force a texture on in blitz. we do not need this shit
    return brush;
}

static MeshModel* parseNode(int nodeIndex, const tinygltf::Model& model,
    const map<int, NodeAnimData>& nodeAnims, MeshModel* parent = nullptr) {

    const tinygltf::Node& node = model.nodes[nodeIndex];
    MeshModel* meshNode = d_new MeshModel();

    if (!node.name.empty()) {
        meshNode->setName(node.name);
        nodes_map[node.name] = meshNode;
    }
    else {
        static int unnamedCount = 0;
        char name[256];
        sprintf(name, "Node_%d", unnamedCount++);
        meshNode->setName(name);
        nodes_map[name] = meshNode;
    }

    if (!node.matrix.empty()) {
        Matrix m(
            Vector((float)node.matrix[0], (float)node.matrix[1], (float)node.matrix[2]),
            Vector((float)node.matrix[4], (float)node.matrix[5], (float)node.matrix[6]),
            Vector((float)node.matrix[8], (float)node.matrix[9], (float)node.matrix[10])
        );
        Vector p((float)node.matrix[12], (float)node.matrix[13], (float)node.matrix[14]);
        Transform tform(m, p);
        if (conv) tform = conv_tform * tform * -conv_tform;
        meshNode->setLocalTform(tform);
    }
    else {
        Vector translation(0, 0, 0);
        Vector scale(1, 1, 1);
        Quat rotation(1, Vector(0, 0, 0));

        if (!node.translation.empty()) {
            translation = Vector((float)node.translation[0], (float)node.translation[1], (float)node.translation[2]);
        }
        if (!node.scale.empty()) {
            scale = Vector((float)node.scale[0], (float)node.scale[1], (float)node.scale[2]);
        }
        if (!node.rotation.empty()) {
            float x = (float)node.rotation[0];
            float y = (float)node.rotation[1];
            float z = (float)node.rotation[2];
            float w = (float)node.rotation[3];
            rotation = Quat(w, Vector(x, y, z));
        }

        Matrix scaleMat = scaleMatrix(scale);
        Matrix rotMat(rotation);
        Transform tform(rotMat * scaleMat, translation);

        if (conv) tform = conv_tform * tform * -conv_tform;
        meshNode->setLocalTform(tform);
    }

    if (parent) {
        meshNode->setParent(parent);
    }

    if (node.mesh >= 0 && node.mesh < model.meshes.size() && !animonly) {
        const auto& mesh = model.meshes[node.mesh];

        for (const auto& primitive : mesh.primitives) {
            if (primitive.mode != TINYGLTF_MODE_TRIANGLES) continue;

            vector<Brush> materials;
            if (primitive.material >= 0 && primitive.material < model.materials.size()) {
                materials.push_back(parseMaterial(model.materials[primitive.material]));
            }
            else {
                materials.push_back(Brush());
            }

            MeshLoader::beginMesh();

            auto posIt = primitive.attributes.find("POSITION");
            auto normalIt = primitive.attributes.find("NORMAL");
            auto texcoordIt = primitive.attributes.find("TEXCOORD_0");
            auto colorIt = primitive.attributes.find("COLOR_0");

            // vertices
            if (posIt != primitive.attributes.end()) {
                const auto& accessor = model.accessors[posIt->second];
                const auto& bufferView = model.bufferViews[accessor.bufferView];
                const auto& buffer = model.buffers[bufferView.buffer];

                const float* positions = reinterpret_cast<const float*>(
                    &buffer.data[bufferView.byteOffset + accessor.byteOffset]);

                for (size_t k = 0; k < accessor.count; ++k) {
                    Surface::Vertex v;
                    v.coords = Vector(positions[k * 3], positions[k * 3 + 1], positions[k * 3 + 2]);
                    if (conv) v.coords = conv_tform * v.coords;
                    v.color = 0xffffffff;
                    MeshLoader::addVertex(v);
                }

                // normals
                if (normalIt != primitive.attributes.end()) {
                    const auto& normalAccessor = model.accessors[normalIt->second];
                    const auto& normalBufferView = model.bufferViews[normalAccessor.bufferView];
                    const auto& normalBuffer = model.buffers[normalBufferView.buffer];

                    const float* normals = reinterpret_cast<const float*>(
                        &normalBuffer.data[normalBufferView.byteOffset + normalAccessor.byteOffset]);

                    Matrix co = conv_tform.m.cofactor();
                    for (size_t k = 0; k < normalAccessor.count; ++k) {
                        Surface::Vertex& v = MeshLoader::refVertex((int)k);
                        v.normal = (co * Vector(normals[k * 3], normals[k * 3 + 1], normals[k * 3 + 2])).normalized();
                    }
                }

                // texture coords
                if (texcoordIt != primitive.attributes.end()) {
                    const auto& texAccessor = model.accessors[texcoordIt->second];
                    const auto& texBufferView = model.bufferViews[texAccessor.bufferView];
                    const auto& texBuffer = model.buffers[texBufferView.buffer];

                    const float* texcoords = reinterpret_cast<const float*>(
                        &texBuffer.data[texBufferView.byteOffset + texAccessor.byteOffset]);

                    int components = texAccessor.type == TINYGLTF_TYPE_VEC2 ? 2 : 1;
                    for (size_t k = 0; k < texAccessor.count; ++k) {
                        Surface::Vertex& v = MeshLoader::refVertex((int)k);
                        float tu = texcoords[k * components];
                        float tv = components > 1 ? texcoords[k * components + 1] : 0.0f;
                        v.tex_coords[0][0] = v.tex_coords[1][0] = tu;
                        v.tex_coords[0][1] = v.tex_coords[1][1] = tv;
                    }
                }

                // vertex colors
                if (colorIt != primitive.attributes.end()) {
                    const auto& colorAccessor = model.accessors[colorIt->second];
                    const auto& colorBufferView = model.bufferViews[colorAccessor.bufferView];
                    const auto& colorBuffer = model.buffers[colorBufferView.buffer];

                    if (colorAccessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
                        const float* colors = reinterpret_cast<const float*>(
                            &colorBuffer.data[colorBufferView.byteOffset + colorAccessor.byteOffset]);

                        int components = colorAccessor.type == TINYGLTF_TYPE_VEC3 ? 3 : 4;
                        for (size_t k = 0; k < colorAccessor.count; ++k) {
                            Surface::Vertex& v = MeshLoader::refVertex((int)k);
                            if (components >= 3) {
                                v.color = 0xff000000 |
                                    ((int)(colors[k * components] * 255) << 16) |
                                    ((int)(colors[k * components + 1] * 255) << 8) |
                                    (int)(colors[k * components + 2] * 255);
                            }
                        }
                    }
                }

                // indices
                const auto& indexAccessor = model.accessors[primitive.indices];
                const auto& indexBufferView = model.bufferViews[indexAccessor.bufferView];
                const auto& indexBuffer = model.buffers[indexBufferView.buffer];

                vector<unsigned int> indices;
                if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                    const unsigned short* idx = reinterpret_cast<const unsigned short*>(
                        &indexBuffer.data[indexBufferView.byteOffset + indexAccessor.byteOffset]);
                    indices.assign(idx, idx + indexAccessor.count);
                }
                else if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                    const unsigned int* idx = reinterpret_cast<const unsigned int*>(
                        &indexBuffer.data[indexBufferView.byteOffset + indexAccessor.byteOffset]);
                    indices.assign(idx, idx + indexAccessor.count);
                }
                else if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                    const unsigned char* idx = reinterpret_cast<const unsigned char*>(
                        &indexBuffer.data[indexBufferView.byteOffset + indexAccessor.byteOffset]);
                    indices.reserve(indexAccessor.count);
                    for (size_t k = 0; k < indexAccessor.count; ++k) {
                        indices.push_back(idx[k]);
                    }
                }

                // make triangles
                for (size_t k = 0; k < indices.size(); k += 3) {
                    int tri[3];
                    if (flip_tris) {
                        tri[0] = (int)indices[k];
                        tri[1] = (int)indices[k + 2];
                        tri[2] = (int)indices[k + 1];
                    }
                    else {
                        tri[0] = (int)indices[k];
                        tri[1] = (int)indices[k + 1];
                        tri[2] = (int)indices[k + 2];
                    }
                    MeshLoader::addTriangle(tri, materials[0]);
                }
            }

            MeshLoader::endMesh(meshNode);
            if (normalIt == primitive.attributes.end()) {
                meshNode->updateNormals();
            }
        }
    }

    auto animIt = nodeAnims.find(nodeIndex);
    if (animIt != nodeAnims.end()) {
        applyAnimationToNode(meshNode, animIt->second);
    }

    for (int childIndex : node.children) {
        if (childIndex >= 0 && childIndex < model.nodes.size()) {
            parseNode(childIndex, model, nodeAnims, meshNode);
        }
    }

    return meshNode;
}

static MeshModel* parseGLTF(const string& filename) {
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    string err;
    string warn;

    bool success = false;
    size_t dotPos = filename.find_last_of(".");
    string ext = dotPos != string::npos ? filename.substr(dotPos + 1) : "";

    loader.SetStoreOriginalJSONForExtrasAndExtensions(false);

    if (ext == "gltf") {
        success = loader.LoadASCIIFromFile(&model, &err, &warn, filename);
    }
    else if (ext == "glb") {
        success = loader.LoadBinaryFromFile(&model, &err, &warn, filename);
    }
    else {
        gx_runtime->debugLog("GLTF Load Error: Unsupported file extension");
        return 0;
    }

    if (!success) {
        if (!err.empty()) {
            gx_runtime->debugLog(("GLTF Load Error: " + err).c_str());
        }
        if (!warn.empty()) {
            gx_runtime->debugLog(("GLTF Load Warning: " + warn).c_str());
        }
        return 0;
    }

    if (model.scenes.empty()) {
        gx_runtime->debugLog("GLTF Load Error: No scenes found in file");
        return 0;
    }

    anim_len = 0;

    map<int, NodeAnimData> nodeAnims;
    if (!collapse) {
        parseAnimations(model, nodeAnims);
    }

    MeshModel* root = d_new MeshModel();
    root->setName("Root");

    int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : 0;
    const auto& scene = model.scenes[sceneIndex];

    for (int nodeIndex : scene.nodes) {
        if (nodeIndex >= 0 && nodeIndex < model.nodes.size()) {
            MeshModel* child = parseNode(nodeIndex, model, nodeAnims);
            child->setParent(root);
        }
    }

    if (!collapse && anim_len > 0) {
        if (anim_len == 0) anim_len = 1;
        root->setAnimator(d_new Animator(root, anim_len));
        gx_runtime->debugLog(("GLTF Load: Created animator with length " + to_string(anim_len) + " ms").c_str());
    }
    else if (!collapse && nodeAnims.empty()) {
        gx_runtime->debugLog("GLTF Load: No animations found in file");
    }

    nodes_map.clear();
    return root;
}

MeshModel* Loader_GLTF::load(const string& filename, const Transform& t, int hint) {
    conv_tform = t;
    conv = flip_tris = false;
    if (conv_tform != Transform()) {
        conv = true;
        if (conv_tform.m.i.cross(conv_tform.m.j).dot(conv_tform.m.k) < 0) flip_tris = true;
    }
    collapse = !!(hint & MeshLoader::HINT_COLLAPSE);
    animonly = !!(hint & MeshLoader::HINT_ANIMONLY);

    gx_runtime->debugLog(("Loading GLTF: " + filename).c_str());
    MeshModel* e = parseGLTF(filename);
    nodes_map.clear();
    return e;
}