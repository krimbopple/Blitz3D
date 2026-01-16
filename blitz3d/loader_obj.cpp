#include "std.h"
#include "loader_obj.h"
#include "meshmodel.h"
#include "animation.h"

#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <set>

using namespace std;

struct TexCoord {
    float u, v;
    TexCoord(float u = 0, float v = 0) : u(u), v(v) {}
};

struct OBJVertex {
    int position;
    int normal;
    int texcoord;

    bool operator<(const OBJVertex& other) const {
        if (position != other.position) return position < other.position;
        if (normal != other.normal) return normal < other.normal;
        return texcoord < other.texcoord;
    }
};

struct OBJMaterial {
    string name;
    Vector diffuse;
    Vector ambient;
    Vector specular;
    float shininess;
    float alpha;
    string texture;
};

static Transform conv_tform;
static bool conv, flip_tris;
static bool collapse, animonly;

static vector<Vector> positions;
static vector<Vector> normals;
static vector<TexCoord> texcoords;
static vector<OBJMaterial> materials;
static map<string, int> material_map;
static int current_material = -1;
static int vertex_count = 0;

static void parseMTL(const string& filename);

static MeshModel* parseOBJ(const string& filename) {
    ifstream file(filename.c_str());
    if (!file.is_open()) {
        return 0;
    }

    positions.clear();
    normals.clear();
    texcoords.clear();
    materials.clear();
    material_map.clear();
    current_material = -1;
    vertex_count = 0;

    positions.push_back(Vector()); // 0 is unused
    texcoords.push_back(TexCoord()); // 0 is unused
    normals.push_back(Vector(0, 0, 1)); // 0 is unused

    OBJMaterial default_mat;
    default_mat.name = "default";
    default_mat.diffuse = Vector(0.8f, 0.8f, 0.8f);
    default_mat.ambient = Vector(0.2f, 0.2f, 0.2f);
    default_mat.specular = Vector(0.0f, 0.0f, 0.0f);
    default_mat.shininess = 0.0f;
    default_mat.alpha = 1.0f;
    default_mat.texture = "";
    materials.push_back(default_mat);
    material_map["default"] = 0;
    current_material = 0;

    MeshModel* root = d_new MeshModel();
    MeshModel* current_mesh = root;

    vector<Surface::Vertex> mesh_vertices;
    vector<int> mesh_triangles;
    vector<Brush> mesh_brushes;
    string current_group = "default";
    map<OBJVertex, int> vertex_map;

    string line;
    int line_num = 0;

    while (getline(file, line)) {
        line_num++;

        size_t comment_pos = line.find('#');
        if (comment_pos != string::npos) {
            line = line.substr(0, comment_pos);
        }

        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        if (line.empty()) continue;

        istringstream iss(line);
        string prefix;
        iss >> prefix;

        if (prefix == "v") {
            Vector pos;
            iss >> pos.x >> pos.y >> pos.z;
            if (conv) {
                pos = conv_tform * pos;
            }
            positions.push_back(pos);
        }
        else if (prefix == "vn") {
            Vector norm;
            iss >> norm.x >> norm.y >> norm.z;
            if (conv) {
                Matrix co = conv_tform.m.cofactor();
                norm = (co * norm).normalized();
            }
            else {
                norm.normalize();
            }
            normals.push_back(norm);
        }
        else if (prefix == "vt") {
            TexCoord tex;
            iss >> tex.u;
            if (!(iss >> tex.v)) {
                tex.v = 0;
            }
            float w;
            if (iss >> w) {
            }

            tex.v = 1.0f - tex.v;
            texcoords.push_back(tex);
        }
        else if (prefix == "f") { // Face
            vector<OBJVertex> face_vertices;
            string token;

            while (iss >> token) {
                OBJVertex v;
                v.position = v.normal = v.texcoord = 0;

                size_t slash1 = token.find('/');
                if (slash1 == string::npos) {
                    v.position = atoi(token.c_str());
                    v.texcoord = 0;
                    v.normal = 0;
                }
                else {
                    string pos_str = token.substr(0, slash1);
                    v.position = pos_str.empty() ? 0 : atoi(pos_str.c_str());

                    size_t slash2 = token.find('/', slash1 + 1);
                    if (slash2 == string::npos) {
                        string tex_str = token.substr(slash1 + 1);
                        v.texcoord = tex_str.empty() ? 0 : atoi(tex_str.c_str());
                        v.normal = 0;
                    }
                    else {
                        string tex_str = token.substr(slash1 + 1, slash2 - slash1 - 1);
                        string norm_str = token.substr(slash2 + 1);

                        v.texcoord = tex_str.empty() ? 0 : atoi(tex_str.c_str());
                        v.normal = norm_str.empty() ? 0 : atoi(norm_str.c_str());
                    }
                }

                if (v.position < 0) v.position = positions.size() + v.position;
                if (v.normal < 0) v.normal = normals.size() + v.normal;
                if (v.texcoord < 0) v.texcoord = texcoords.size() + v.texcoord;

                if (v.position < 0 || v.position >= (int)positions.size()) v.position = 0;
                if (v.normal < 0 || v.normal >= (int)normals.size()) v.normal = 0;
                if (v.texcoord < 0 || v.texcoord >= (int)texcoords.size()) v.texcoord = 0;

                face_vertices.push_back(v);
            }

            // triangulate polygon if more than 3 verts
            if (face_vertices.size() >= 3) {
                for (size_t i = 1; i < face_vertices.size() - 1; i++) {
                    OBJVertex* face_v[3] = { &face_vertices[0], &face_vertices[i], &face_vertices[i + 1] };
                    int tri_indices[3];

                    for (int j = 0; j < 3; j++) {
                        OBJVertex v_key = *face_v[j];

                        map<OBJVertex, int>::iterator it = vertex_map.find(v_key);
                        if (it != vertex_map.end()) {
                            tri_indices[j] = it->second;
                        }
                        else {
                            Surface::Vertex vertex;

                            vertex.coords = positions[v_key.position];

                            if (v_key.texcoord > 0 && v_key.texcoord < (int)texcoords.size()) {
                                vertex.tex_coords[0][0] = vertex.tex_coords[1][0] = texcoords[v_key.texcoord].u;
                                vertex.tex_coords[0][1] = vertex.tex_coords[1][1] = texcoords[v_key.texcoord].v;
                            }
                            else {
                                vertex.tex_coords[0][0] = vertex.tex_coords[1][0] = 0;
                                vertex.tex_coords[0][1] = vertex.tex_coords[1][1] = 0;
                            }

                            if (v_key.normal > 0 && v_key.normal < (int)normals.size()) {
                                vertex.normal = normals[v_key.normal];
                            }
                            else {
                                vertex.normal = Vector(0, 0, 1);
                            }

                            vertex.color = 0xffffffff;

                            mesh_vertices.push_back(vertex);
                            int new_index = vertex_count++;
                            vertex_map[v_key] = new_index;
                            tri_indices[j] = new_index;
                        }
                    }

                    if (flip_tris) {
                        swap(tri_indices[1], tri_indices[2]);
                    }

                    mesh_triangles.push_back(tri_indices[0]);
                    mesh_triangles.push_back(tri_indices[1]);
                    mesh_triangles.push_back(tri_indices[2]);

                    Brush brush;
                    if (current_material >= 0 && current_material < (int)materials.size()) {
                        OBJMaterial& mat = materials[current_material];
                        brush.setColor(mat.diffuse);
                        brush.setAlpha(mat.alpha);
                        if (!mat.texture.empty()) {
                            brush.setTexture(0, Texture(mat.texture, 0), 0);
                            brush.setColor(Vector(1, 1, 1));
                        }
                    }
                    mesh_brushes.push_back(brush);
                }
            }
        }
        else if (prefix == "usemtl") {
            string mat_name;
            iss >> mat_name;

            map<string, int>::iterator it = material_map.find(mat_name);
            if (it != material_map.end()) {
                current_material = it->second;
            }
            else {
                current_material = 0;
            }
        }
        else if (prefix == "mtllib") {
            string mtl_filename;
            iss >> mtl_filename;

            string path = filename;
            size_t last_slash = path.find_last_of("/\\");
            if (last_slash != string::npos) {
                path = path.substr(0, last_slash + 1);
            }
            else {
                path = "";
            }

            parseMTL(path + mtl_filename);
        }
        else if (prefix == "g" || prefix == "o") {
            if (!mesh_vertices.empty()) {
                MeshModel* mesh = d_new MeshModel();
                mesh->setName(current_group);

                MeshLoader::beginMesh();

                for (size_t i = 0; i < mesh_vertices.size(); i++) {
                    MeshLoader::addVertex(mesh_vertices[i]);
                }

                for (size_t i = 0; i < mesh_triangles.size(); i += 3) {
                    int tri[3] = { mesh_triangles[i], mesh_triangles[i + 1], mesh_triangles[i + 2] };
                    MeshLoader::addTriangle(tri, mesh_brushes[i / 3]);
                }

                MeshLoader::endMesh(mesh);

                bool has_normals = true;
                for (size_t i = 0; i < mesh_vertices.size(); i++) {
                    if (mesh_vertices[i].normal.length() < 0.9f) {
                        has_normals = false;
                        break;
                    }
                }
                if (!has_normals) {
                    mesh->updateNormals();
                }

                mesh->setParent(root);
                mesh_vertices.clear();
                mesh_triangles.clear();
                mesh_brushes.clear();
                vertex_map.clear();
                vertex_count = 0;
            }

            if (iss >> current_group) {
            }
            else {
                current_group = "unnamed";
            }
        }
    }

    file.close();

    if (!mesh_vertices.empty()) {
        MeshModel* mesh = d_new MeshModel();
        mesh->setName(current_group);

        MeshLoader::beginMesh();

        // vertices
        for (size_t i = 0; i < mesh_vertices.size(); i++) {
            MeshLoader::addVertex(mesh_vertices[i]);
        }

        // triangles
        for (size_t i = 0; i < mesh_triangles.size(); i += 3) {
            int tri[3] = { mesh_triangles[i], mesh_triangles[i + 1], mesh_triangles[i + 2] };
            MeshLoader::addTriangle(tri, mesh_brushes[i / 3]);
        }

        MeshLoader::endMesh(mesh);

        bool has_normals = true;
        for (size_t i = 0; i < mesh_vertices.size(); i++) {
            if (mesh_vertices[i].normal.length() < 0.9f) {
                has_normals = false;
                break;
            }
        }
        if (!has_normals) {
            mesh->updateNormals();
        }

        mesh->setParent(root);
    }

    return root;
}

static void parseMTL(const string& filename) {
    ifstream file(filename.c_str());
    if (!file.is_open()) {
        return;
    }

    OBJMaterial current_mat;
    bool has_current = false;

    string line;
    while (getline(file, line)) {
        size_t comment_pos = line.find('#');
        if (comment_pos != string::npos) {
            line = line.substr(0, comment_pos);
        }

        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        if (line.empty()) continue;

        istringstream iss(line);
        string prefix;
        iss >> prefix;

        if (prefix == "newmtl") {
            if (has_current) {
                materials.push_back(current_mat);
                material_map[current_mat.name] = materials.size() - 1;
            }

            iss >> current_mat.name;
            current_mat.diffuse = Vector(0.8f, 0.8f, 0.8f);
            current_mat.ambient = Vector(0.2f, 0.2f, 0.2f);
            current_mat.specular = Vector(0.0f, 0.0f, 0.0f);
            current_mat.shininess = 0.0f;
            current_mat.alpha = 1.0f;
            current_mat.texture = "";
            has_current = true;
        }
        else if (prefix == "Kd" && has_current) {
            iss >> current_mat.diffuse.x >> current_mat.diffuse.y >> current_mat.diffuse.z;
        }
        else if (prefix == "Ka" && has_current) {
            iss >> current_mat.ambient.x >> current_mat.ambient.y >> current_mat.ambient.z;
        }
        else if (prefix == "Ks" && has_current) {
            iss >> current_mat.specular.x >> current_mat.specular.y >> current_mat.specular.z;
        }
        else if (prefix == "Ns" && has_current) {
            iss >> current_mat.shininess;
        }
        else if (prefix == "d" && has_current) {
            iss >> current_mat.alpha;
        }
        else if (prefix == "Tr" && has_current) {
            float transparency;
            iss >> transparency;
            current_mat.alpha = 1.0f - transparency;
        }
        else if (prefix == "map_Kd" && has_current) {
            string texture_file;
            iss >> texture_file;
            current_mat.texture = texture_file;
        }
    }

    if (has_current) {
        materials.push_back(current_mat);
        material_map[current_mat.name] = materials.size() - 1;
    }

    file.close();
}

MeshModel* Loader_OBJ::load(const string& filename, const Transform& transform, int hint) {
    conv_tform = transform;
    conv = flip_tris = false;

    if (conv_tform != Transform()) {
        conv = true;
        if (conv_tform.m.i.cross(conv_tform.m.j).dot(conv_tform.m.k) < 0) {
            flip_tris = true;
        }
    }

    collapse = !!(hint & MeshLoader::HINT_COLLAPSE);
    animonly = !!(hint & MeshLoader::HINT_ANIMONLY);

    if (animonly) {
        return d_new MeshModel();
    }

    return parseOBJ(filename);
}