#ifndef LOADER_OBJ_H
#define LOADER_OBJ_H

#include "meshloader.h"

class Loader_OBJ : public MeshLoader {
public:
    MeshModel* load(const string & filename, const Transform& transform, int hint);
};

#endif