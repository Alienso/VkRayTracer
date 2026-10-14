//
// The material and punctual light values the editor can change, listed once. The scene file saves the ones that
// differ from the model file (SceneFile.cpp), and AssetStore::resetEdits puts the file's back. A value added to the
// editor's material or light panel goes here too, or its edits aren't saved.
//
// X(name) for each: Material::name for MATERIAL_FIELDS, SceneMaterial::name for MATERIAL_SWITCHES (the "use texture"
// switches), PunctualLight::name for LIGHT_FIELDS. Not in the lists: texture indices, names, UV transforms and the
// light's place in the model (they only come from the file).
//

#ifndef VULKANDEMO_EDITABLEFIELDS_H
#define VULKANDEMO_EDITABLEFIELDS_H

#define MATERIAL_FIELDS(X)                                                                                              \
    X(baseColor) X(metallic) X(roughness) X(transmission) X(ior) X(solid) X(attenuationColor) X(attenuationDistance) \
    X(diffuseTransmission) X(diffuseTransmissionColor) X(fogDensity) X(fogColor) X(fogForward) X(invisibleSurface)   \
    X(fogNoise) X(fogNoiseSize) X(fogCoverage) X(fogNoiseSeed) X(fogEdge) X(fogDrift) X(emissiveColor)                \
    X(emissiveStrength) X(alphaMode) X(alphaCutoff) X(normalScale)

#define MATERIAL_SWITCHES(X)                                                                                            \
    X(useBaseColorTexture) X(useMetallicRoughnessTexture) X(useTransmissionTexture) X(useEmissiveTexture)            \
    X(useNormalTexture) X(useDiffuseTransmissionTexture) X(useDiffuseTransmissionColorTexture)

#define LIGHT_FIELDS(X) X(enabled) X(color) X(intensity) X(range) X(innerConeAngle) X(outerConeAngle)

#endif //VULKANDEMO_EDITABLEFIELDS_H
