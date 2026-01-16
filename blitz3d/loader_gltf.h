#ifndef LOADER_GLTF_H
#define LOADER_GLTF_H

#include "meshloader.h"

class Loader_GLTF : public MeshLoader {
public:
    MeshModel* load(const string& filename, const Transform& conv, int hint);
};

#endif