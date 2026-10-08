//
// Models built in code instead of loaded from a file: test scenes such as the Cornell box.
// They come out as an ordinary Model, so the rest of the program treats them like loaded files.
//

#ifndef VULKANDEMO_BUILTINMODELS_H
#define VULKANDEMO_BUILTINMODELS_H

#include <string>
#include <vector>
#include "assets/ModelLoader.h"

// Names shown in the "Add" list
const std::vector<std::string>& builtInModelNames();
bool isBuiltInModel(const std::string& name);

// Unlike files, built-in models are already in world units, standing on y = 0 and centered at the origin.
Model createBuiltInModel(const std::string& name);

#endif //VULKANDEMO_BUILTINMODELS_H
