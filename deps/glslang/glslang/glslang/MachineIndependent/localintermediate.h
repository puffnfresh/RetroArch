//
// Copyright (C) 2002-2005  3Dlabs Inc. Ltd.
// Copyright (C) 2016 LunarG, Inc.
// Copyright (C) 2017 ARM Limited.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//    Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//
//    Redistributions in binary form must reproduce the above
//    copyright notice, this list of conditions and the following
//    disclaimer in the documentation and/or other materials provided
//    with the distribution.
//
//    Neither the name of 3Dlabs Inc. Ltd. nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//

#ifndef _LOCAL_INTERMEDIATE_INCLUDED_
#define _LOCAL_INTERMEDIATE_INCLUDED_

#include "../Include/intermediate.h"
#include "../Public/ShaderLang.h"
#include "Versions.h"

#include <algorithm>
#include <set>
#include <array>

class TInfoSink;

namespace glslang {

struct TMatrixSelector {
    int coord1;  // stay agnostic about column/row; this is parse order
    int coord2;
};

typedef int TVectorSelector;

const int MaxSwizzleSelectors = 4;

template<typename selectorType>
class TSwizzleSelectors {
public:
    TSwizzleSelectors() : size_(0) { }

    void push_back(selectorType comp)
    {
        if (size_ < MaxSwizzleSelectors)
            components[size_++] = comp;
    }
    void resize(int s) { size_ = s; }
    int size() const { return size_; }
    selectorType operator[](int i) const
    {
        assert(i < MaxSwizzleSelectors);
        return components[i];
    }

private:
    int size_;
    selectorType components[MaxSwizzleSelectors+1];
};

//
// Some helper structures for TIntermediate.  Their contents are encapsulated
// by TIntermediate.
//

// Used for call-graph algorithms for detecting recursion, missing bodies, and dead bodies.
// A "call" is a pair: <caller, callee>.
// There can be duplicates. General assumption is the list is small.
struct TCall {
    TCall(const TString& pCaller, const TString& pCallee) : caller(pCaller), callee(pCallee) { }
    TString caller;
    TString callee;
    bool visited;
    bool currentPath;
    bool errorGiven;
    int calleeBodyPosition;
};

// A generic 1-D range.
struct TRange {
    TRange(int start, int last) : start(start), last(last) { }
    bool overlap(const TRange& rhs) const
    {
        return last >= rhs.start && start <= rhs.last;
    }
    int start;
    int last;
};

// An IO range is a 3-D rectangle; the set of (location, component, index) triples all lying
// within the same location range, component range, and index value.  Locations don't alias unless
// all other dimensions of their range overlap.
struct TIoRange {
    TIoRange(TRange location, TRange component, TBasicType basicType, int index)
        : location(location), component(component), basicType(basicType), index(index) { }
    bool overlap(const TIoRange& rhs) const
    {
        return location.overlap(rhs.location) && component.overlap(rhs.component) && index == rhs.index;
    }
    TRange location;
    TRange component;
    TBasicType basicType;
    int index;
};

// An offset range is a 2-D rectangle; the set of (binding, offset) pairs all lying
// within the same binding and offset range.
struct TOffsetRange {
    TOffsetRange(TRange binding, TRange offset)
        : binding(binding), offset(offset) { }
    bool overlap(const TOffsetRange& rhs) const
    {
        return binding.overlap(rhs.binding) && offset.overlap(rhs.offset);
    }
    TRange binding;
    TRange offset;
};

// Things that need to be tracked per xfb buffer.
struct TXfbBuffer {
    TXfbBuffer() : stride(TQualifier::layoutXfbStrideEnd), implicitStride(0), containsDouble(false) { }
    std::vector<TRange> ranges;  // byte offsets that have already been assigned
    unsigned int stride;
    unsigned int implicitStride;
    bool containsDouble;
};

// Track a set of strings describing how the module was processed.
// Using the form:
//   process arg0 arg1 arg2 ...
//   process arg0 arg1 arg2 ...
// where everything is textual, and there can be zero or more arguments
class TProcesses {
public:
    TProcesses() {}
    ~TProcesses() {}

    void addArgument(int arg)
    {
        processes.back().append(" ");
        std::string argString = std::to_string(arg);
        processes.back().append(argString);
    }
    void addArgument(const char* arg)
    {
        processes.back().append(" ");
        processes.back().append(arg);
    }
    std::vector<std::string> processes;
};

class TSymbolTable;
class TSymbol;
class TVariable;

//
// Set of helper functions to help parse and build the tree.
//
class TIntermediate {
public:
    explicit TIntermediate(EShLanguage l, int v = 0, EProfile p = ENoProfile) :
        implicitThisName("@this"), implicitCounterName("@count"),
        language(l), source(EShSourceNone), profile(p), version(v), treeRoot(0),
        numEntryPoints(0), numErrors(0), numPushConstants(0), recursive(false),
        invocations(TQualifier::layoutNotSet), vertices(TQualifier::layoutNotSet),
        inputPrimitive(ElgNone), outputPrimitive(ElgNone),
        pixelCenterInteger(false), originUpperLeft(false),
        vertexSpacing(EvsNone), vertexOrder(EvoNone), pointMode(false), earlyFragmentTests(false),
        postDepthCoverage(false), depthLayout(EldNone), depthReplacing(false),
        hlslFunctionality1(false),
        blendEquations(0), xfbMode(false), multiStream(false),
#ifdef NV_EXTENSIONS
        layoutOverrideCoverage(false),
        geoPassthroughEXT(false),
#endif
        autoMapBindings(false),
        autoMapLocations(false),
        invertY(false),
        flattenUniformArrays(false),
        useUnknownFormat(false),
        hlslOffsets(false),
        useStorageBuffer(false),
        textureSamplerTransformMode(EShTexSampTransKeep),
        needToLegalize(false),
        binaryDoubleOutput(false)
    {
        localSize[0] = 1;
        localSize[1] = 1;
        localSize[2] = 1;
        localSizeSpecId[0] = TQualifier::layoutNotSet;
        localSizeSpecId[1] = TQualifier::layoutNotSet;
        localSizeSpecId[2] = TQualifier::layoutNotSet;
        xfbBuffers.resize(TQualifier::layoutXfbBufferEnd);
    }
    void setLimits(const TBuiltInResource& r) { resources = r; }

    bool postProcess(TIntermNode*, EShLanguage);

    void setSource(EShSource s) { source = s; }
    EShSource getSource() const { return source; }
    void setEntryPointName(const char* ep)
    {
        entryPointName = ep;
        processes.processes.push_back("entry-point");
        processes.addArgument(entryPointName.c_str());
    }
    void setEntryPointMangledName(const char* ep) { entryPointMangledName = ep; }
    const std::string& getEntryPointName() const { return entryPointName; }
    const std::string& getEntryPointMangledName() const { return entryPointMangledName; }

    const std::vector<std::string>& getResourceSetBinding() const { return resourceSetBinding; }
    bool getAutoMapBindings() const { return autoMapBindings; }
    bool getAutoMapLocations() const { return autoMapLocations; }
    bool getInvertY() const { return invertY; }

    bool getFlattenUniformArrays() const { return flattenUniformArrays; }
    bool getNoStorageFormat() const { return useUnknownFormat; }
    void setHlslOffsets()
    {
        hlslOffsets = true;
        if (hlslOffsets)
            processes.processes.push_back("hlsl-offsets");
    }
    bool usingHlslOFfsets() const { return hlslOffsets; }
    void setUseStorageBuffer()
    {
        useStorageBuffer = true;
        processes.processes.push_back("use-storage-buffer");
    }
    bool usingStorageBuffer() const { return useStorageBuffer; }

    template<class T> T addCounterBufferName(const T& name) const { return name + implicitCounterName; }
    bool hasCounterBufferName(const TString& name) const {
        size_t len = strlen(implicitCounterName);
        return name.size() > len &&
               name.compare(name.size() - len, len, implicitCounterName) == 0;
    }

    void setVersion(int v) { version = v; }
    int getVersion() const { return version; }
    void setProfile(EProfile p) { profile = p; }
    EProfile getProfile() const { return profile; }
    void setSpv(const SpvVersion& s)
    {
        spvVersion = s;

        // client processes
        if (spvVersion.vulkan > 0)
            processes.processes.push_back("client vulkan100");
        if (spvVersion.openGl > 0)
            processes.processes.push_back("client opengl100");

        // target-environment processes
        if (spvVersion.vulkan > 0)
            processes.processes.push_back("target-env vulkan1.0");
        else if (spvVersion.vulkan > 0)
            processes.processes.push_back("target-env vulkanUnknown");
        if (spvVersion.openGl > 0)
            processes.processes.push_back("target-env opengl");
    }
    const SpvVersion& getSpv() const { return spvVersion; }
    EShLanguage getStage() const { return language; }
    void addRequestedExtension(const char* extension) { requestedExtensions.insert(extension); }
    const std::set<std::string>& getRequestedExtensions() const { return requestedExtensions; }

    void setTreeRoot(TIntermNode* r) { treeRoot = r; }
    TIntermNode* getTreeRoot() const { return treeRoot; }
    void incrementEntryPointCount() { ++numEntryPoints; }
    int getNumEntryPoints() const { return numEntryPoints; }
    int getNumErrors() const { return numErrors; }
    void addPushConstantCount() { ++numPushConstants; }
    bool isRecursive() const { return recursive; }

    TIntermSymbol* addSymbol(const TVariable&);
    TIntermSymbol* addSymbol(const TVariable&, const TSourceLoc&);
    TIntermSymbol* addSymbol(const TType&, const TSourceLoc&);
    TIntermSymbol* addSymbol(const TIntermSymbol&);
    TIntermTyped* addConversion(TOperator, const TType&, TIntermTyped*) const;
    std::tuple<TIntermTyped*, TIntermTyped*> addConversion(TOperator op, TIntermTyped* node0, TIntermTyped* node1) const;
    TIntermTyped* addUniShapeConversion(TOperator, const TType&, TIntermTyped*);
    void addBiShapeConversion(TOperator, TIntermTyped*& lhsNode, TIntermTyped*& rhsNode);
    TIntermTyped* addShapeConversion(const TType&, TIntermTyped*);
    TIntermTyped* addBinaryMath(TOperator, TIntermTyped* left, TIntermTyped* right, TSourceLoc);
    TIntermTyped* addAssign(TOperator op, TIntermTyped* left, TIntermTyped* right, TSourceLoc);
    TIntermTyped* addIndex(TOperator op, TIntermTyped* base, TIntermTyped* index, TSourceLoc);
    TIntermTyped* addUnaryMath(TOperator, TIntermTyped* child, TSourceLoc);
    TIntermTyped* addBuiltInFunctionCall(const TSourceLoc& line, TOperator, bool unary, TIntermNode*, const TType& returnType);
    bool canImplicitlyPromote(TBasicType from, TBasicType to, TOperator op = EOpNull) const;
    bool isIntegralPromotion(TBasicType from, TBasicType to) const;
    bool isFPPromotion(TBasicType from, TBasicType to) const;
    bool isIntegralConversion(TBasicType from, TBasicType to) const;
    bool isFPConversion(TBasicType from, TBasicType to) const;
    bool isFPIntegralConversion(TBasicType from, TBasicType to) const;
    TOperator mapTypeToConstructorOp(const TType&) const;
    TIntermAggregate* growAggregate(TIntermNode* left, TIntermNode* right);
    TIntermAggregate* growAggregate(TIntermNode* left, TIntermNode* right, const TSourceLoc&);
    TIntermAggregate* makeAggregate(TIntermNode* node);
    TIntermAggregate* makeAggregate(TIntermNode* node, const TSourceLoc&);
    TIntermAggregate* makeAggregate(const TSourceLoc&);
    TIntermTyped* setAggregateOperator(TIntermNode*, TOperator, const TType& type, TSourceLoc);
    bool areAllChildConst(TIntermAggregate* aggrNode);
    TIntermSelection* addSelection(TIntermTyped* cond, TIntermNodePair code, const TSourceLoc&);
    TIntermTyped* addSelection(TIntermTyped* cond, TIntermTyped* trueBlock, TIntermTyped* falseBlock, const TSourceLoc&);
    TIntermTyped* addComma(TIntermTyped* left, TIntermTyped* right, const TSourceLoc&);
    TIntermTyped* addMethod(TIntermTyped*, const TType&, const TString*, const TSourceLoc&);
    TIntermConstantUnion* addConstantUnion(const TConstUnionArray&, const TType&, const TSourceLoc&, bool literal = false) const;
    TIntermConstantUnion* addConstantUnion(signed char, const TSourceLoc&, bool literal = false) const;
    TIntermConstantUnion* addConstantUnion(unsigned char, const TSourceLoc&, bool literal = false) const;
    TIntermConstantUnion* addConstantUnion(signed short, const TSourceLoc&, bool literal = false) const;
    TIntermConstantUnion* addConstantUnion(unsigned short, const TSourceLoc&, bool literal = false) const;
    TIntermConstantUnion* addConstantUnion(int, const TSourceLoc&, bool literal = false) const;
    TIntermConstantUnion* addConstantUnion(unsigned int, const TSourceLoc&, bool literal = false) const;
    TIntermConstantUnion* addConstantUnion(long long, const TSourceLoc&, bool literal = false) const;
    TIntermConstantUnion* addConstantUnion(unsigned long long, const TSourceLoc&, bool literal = false) const;
    TIntermConstantUnion* addConstantUnion(bool, const TSourceLoc&, bool literal = false) const;
    TIntermConstantUnion* addConstantUnion(double, TBasicType, const TSourceLoc&, bool literal = false) const;
    TIntermConstantUnion* addConstantUnion(const TString*, const TSourceLoc&, bool literal = false) const;
    TIntermTyped* promoteConstantUnion(TBasicType, TIntermConstantUnion*) const;
    bool parseConstTree(TIntermNode*, TConstUnionArray, TOperator, const TType&, bool singleConstantParam = false);
    TIntermLoop* addLoop(TIntermNode*, TIntermTyped*, TIntermTyped*, bool testFirst, const TSourceLoc&);
    TIntermAggregate* addForLoop(TIntermNode*, TIntermNode*, TIntermTyped*, TIntermTyped*, bool testFirst,
        const TSourceLoc&, TIntermLoop*&);
    TIntermBranch* addBranch(TOperator, const TSourceLoc&);
    TIntermBranch* addBranch(TOperator, TIntermTyped*, const TSourceLoc&);
    template<typename selectorType> TIntermTyped* addSwizzle(TSwizzleSelectors<selectorType>&, const TSourceLoc&);

    // Low level functions to add nodes (no conversions or other higher level transformations)
    // If a type is provided, the node's type will be set to it.
    TIntermBinary* addBinaryNode(TOperator op, TIntermTyped* left, TIntermTyped* right, TSourceLoc) const;
    TIntermBinary* addBinaryNode(TOperator op, TIntermTyped* left, TIntermTyped* right, TSourceLoc, const TType&) const;
    TIntermUnary* addUnaryNode(TOperator op, TIntermTyped* child, TSourceLoc) const;
    TIntermUnary* addUnaryNode(TOperator op, TIntermTyped* child, TSourceLoc, const TType&) const;

    // Constant folding (in Constant.cpp)
    TIntermTyped* fold(TIntermAggregate* aggrNode);
    TIntermTyped* foldConstructor(TIntermAggregate* aggrNode);
    TIntermTyped* foldDereference(TIntermTyped* node, int index, const TSourceLoc&);
    TIntermTyped* foldSwizzle(TIntermTyped* node, TSwizzleSelectors<TVectorSelector>& fields, const TSourceLoc&);

    // Tree ops
    static const TIntermTyped* findLValueBase(const TIntermTyped*, bool swizzleOkay);

    // Linkage related
    void addSymbolLinkageNodes(TIntermAggregate*& linkage, EShLanguage, TSymbolTable&);
    void addSymbolLinkageNode(TIntermAggregate*& linkage, const TSymbol&);

    bool setInvocations(int i)
    {
        if (invocations != TQualifier::layoutNotSet)
            return invocations == i;
        invocations = i;
        return true;
    }
    int getInvocations() const { return invocations; }
    bool setVertices(int m)
    {
        if (vertices != TQualifier::layoutNotSet)
            return vertices == m;
        vertices = m;
        return true;
    }
    int getVertices() const { return vertices; }
    bool setInputPrimitive(TLayoutGeometry p)
    {
        if (inputPrimitive != ElgNone)
            return inputPrimitive == p;
        inputPrimitive = p;
        return true;
    }
    TLayoutGeometry getInputPrimitive() const { return inputPrimitive; }
    bool setVertexSpacing(TVertexSpacing s)
    {
        if (vertexSpacing != EvsNone)
            return vertexSpacing == s;
        vertexSpacing = s;
        return true;
    }
    TVertexSpacing getVertexSpacing() const { return vertexSpacing; }
    bool setVertexOrder(TVertexOrder o)
    {
        if (vertexOrder != EvoNone)
            return vertexOrder == o;
        vertexOrder = o;
        return true;
    }
    TVertexOrder getVertexOrder() const { return vertexOrder; }
    void setPointMode() { pointMode = true; }
    bool getPointMode() const { return pointMode; }

    bool setLocalSize(int dim, int size)
    {
        if (localSize[dim] > 1)
            return size == localSize[dim];
        localSize[dim] = size;
        return true;
    }
    unsigned int getLocalSize(int dim) const { return localSize[dim]; }

    bool setLocalSizeSpecId(int dim, int id)
    {
        if (localSizeSpecId[dim] != TQualifier::layoutNotSet)
            return id == localSizeSpecId[dim];
        localSizeSpecId[dim] = id;
        return true;
    }
    int getLocalSizeSpecId(int dim) const { return localSizeSpecId[dim]; }

    void setXfbMode() { xfbMode = true; }
    bool getXfbMode() const { return xfbMode; }
    void setMultiStream() { multiStream = true; }
    bool isMultiStream() const { return multiStream; }
    bool setOutputPrimitive(TLayoutGeometry p)
    {
        if (outputPrimitive != ElgNone)
            return outputPrimitive == p;
        outputPrimitive = p;
        return true;
    }
    TLayoutGeometry getOutputPrimitive() const { return outputPrimitive; }
    void setOriginUpperLeft() { originUpperLeft = true; }
    bool getOriginUpperLeft() const { return originUpperLeft; }
    void setPixelCenterInteger() { pixelCenterInteger = true; }
    bool getPixelCenterInteger() const { return pixelCenterInteger; }
    void setEarlyFragmentTests() { earlyFragmentTests = true; }
    bool getEarlyFragmentTests() const { return earlyFragmentTests; }
    void setPostDepthCoverage() { postDepthCoverage = true; }
    bool getPostDepthCoverage() const { return postDepthCoverage; }
    bool setDepth(TLayoutDepth d)
    {
        if (depthLayout != EldNone)
            return depthLayout == d;
        depthLayout = d;
        return true;
    }
    TLayoutDepth getDepth() const { return depthLayout; }
    void setDepthReplacing() { depthReplacing = true; }
    bool isDepthReplacing() const { return depthReplacing; }

    void setHlslFunctionality1() { hlslFunctionality1 = true; }
    bool getHlslFunctionality1() const { return hlslFunctionality1; }

    void addBlendEquation(TBlendEquationShift b) { blendEquations |= (1 << b); }
    unsigned int getBlendEquations() const { return blendEquations; }

    void addToCallGraph(TInfoSink&, const TString& caller, const TString& callee);
    void merge(TInfoSink&, TIntermediate&);
    void finalCheck(TInfoSink&);

    void addIoAccessed(const TString& name) { ioAccessed.insert(name); }
    bool inIoAccessed(const TString& name) const { return ioAccessed.find(name) != ioAccessed.end(); }

    int addUsedLocation(const TQualifier&, const TType&, bool& typeCollision);
    int checkLocationRange(int set, const TIoRange& range, const TType&, bool& typeCollision);
    int addUsedOffsets(int binding, int offset, int numOffsets);
    bool addUsedConstantId(int id);
    static int computeTypeLocationSize(const TType&, EShLanguage);
    static int computeTypeUniformLocationSize(const TType&);

    bool setXfbBufferStride(int buffer, unsigned stride)
    {
        if (xfbBuffers[buffer].stride != TQualifier::layoutXfbStrideEnd)
            return xfbBuffers[buffer].stride == stride;
        xfbBuffers[buffer].stride = stride;
        return true;
    }
    unsigned getXfbStride(int buffer) const { return xfbBuffers[buffer].stride; }
    int addXfbBufferOffset(const TType&);
    unsigned int computeTypeXfbSize(const TType&, bool& containsDouble) const;
    static int getBaseAlignmentScalar(const TType&, int& size);
    static int getBaseAlignment(const TType&, int& size, int& stride, bool std140, bool rowMajor);
    static bool improperStraddle(const TType& type, int size, int offset);
    bool promote(TIntermOperator*);

#ifdef NV_EXTENSIONS
    void setLayoutOverrideCoverage() { layoutOverrideCoverage = true; }
    bool getLayoutOverrideCoverage() const { return layoutOverrideCoverage; }
    void setGeoPassthroughEXT() { geoPassthroughEXT = true; }
    bool getGeoPassthroughEXT() const { return geoPassthroughEXT; }
#endif

    const char* addSemanticName(const TString& name)
    {
        return semanticNameSet.insert(name).first->c_str();
    }

    void addProcessArgument(const std::string& arg) { processes.addArgument(arg.c_str()); }
    void setNeedsLegalization() { needToLegalize = true; }

    void setBinaryDoubleOutput() { binaryDoubleOutput = true; }

    const char* const implicitThisName;
    const char* const implicitCounterName;
    // for OpModuleProcessed, or equivalent
    TProcesses processes;

protected:
    TIntermSymbol* addSymbol(int Id, const TString&, const TType&, const TConstUnionArray&, TIntermTyped* subtree, const TSourceLoc&);
    void error(TInfoSink& infoSink, const char*);
    void warn(TInfoSink& infoSink, const char*);
    void mergeBodies(TInfoSink&, TIntermSequence& globals, const TIntermSequence& unitGlobals);
    void mergeLinkerObjects(TInfoSink&, TIntermSequence& linkerObjects, const TIntermSequence& unitLinkerObjects);
    void mergeImplicitArraySizes(TType&, const TType&);
    void mergeErrorCheck(TInfoSink&, const TIntermSymbol&, const TIntermSymbol&, bool crossStage);
    void checkCallGraphCycles(TInfoSink&);
    void checkCallGraphBodies(TInfoSink&);
    void inOutLocationCheck(TInfoSink&);
    TIntermSequence& findLinkerObjects() const;
    bool userOutputUsed() const;
    bool isSpecializationOperation(const TIntermOperator&) const;
    bool isNonuniformPropagating(TOperator) const;
    bool promoteUnary(TIntermUnary&);
    bool promoteBinary(TIntermBinary&);
    void addSymbolLinkageNode(TIntermAggregate*& linkage, TSymbolTable&, const TString&);
    bool promoteAggregate(TIntermAggregate&);
    void pushSelector(TIntermSequence&, const TVectorSelector&, const TSourceLoc&);
    void pushSelector(TIntermSequence&, const TMatrixSelector&, const TSourceLoc&);
    bool specConstantPropagates(const TIntermTyped&, const TIntermTyped&);
    void performTextureUpgradeAndSamplerRemovalTransformation(TIntermNode* root);
    bool isConversionAllowed(TOperator op, TIntermTyped* node) const;
    TIntermUnary* createConversion(TBasicType convertTo, TIntermTyped* node) const;
    std::tuple<TBasicType, TBasicType> getConversionDestinatonType(TBasicType type0, TBasicType type1, TOperator op) const;
    bool extensionRequested(const char *extension) const {return requestedExtensions.find(extension) != requestedExtensions.end();}

    const EShLanguage language;  // stage, known at construction time
    EShSource source;            // source language, known a bit later
    std::string entryPointName;
    std::string entryPointMangledName;

    EProfile profile;                           // source profile
    int version;                                // source version
    SpvVersion spvVersion;
    TIntermNode* treeRoot;
    std::set<std::string> requestedExtensions;  // cumulation of all enabled or required extensions; not connected to what subset of the shader used them
    TBuiltInResource resources;
    int numEntryPoints;
    int numErrors;
    int numPushConstants;
    bool recursive;
    int invocations;
    int vertices;
    TLayoutGeometry inputPrimitive;
    TLayoutGeometry outputPrimitive;
    bool pixelCenterInteger;
    bool originUpperLeft;
    TVertexSpacing vertexSpacing;
    TVertexOrder vertexOrder;
    bool pointMode;
    int localSize[3];
    int localSizeSpecId[3];
    bool earlyFragmentTests;
    bool postDepthCoverage;
    TLayoutDepth depthLayout;
    bool depthReplacing;
    bool hlslFunctionality1;
    int blendEquations;        // an 'or'ing of masks of shifts of TBlendEquationShift
    bool xfbMode;
    bool multiStream;

#ifdef NV_EXTENSIONS
    bool layoutOverrideCoverage;
    bool geoPassthroughEXT;
#endif

    std::vector<std::string> resourceSetBinding;
    bool autoMapBindings;
    bool autoMapLocations;
    bool invertY;
    bool flattenUniformArrays;
    bool useUnknownFormat;
    bool hlslOffsets;
    bool useStorageBuffer;

    typedef std::list<TCall> TGraph;
    TGraph callGraph;

    std::set<TString> ioAccessed;           // set of names of statically read/written I/O that might need extra checking
    std::vector<TIoRange> usedIo[4];        // sets of used locations, one for each of in, out, uniform, and buffers
    std::vector<TOffsetRange> usedAtomics;  // sets of bindings used by atomic counters
    std::vector<TXfbBuffer> xfbBuffers;     // all the data we need to track per xfb buffer
    std::unordered_set<int> usedConstantId; // specialization constant ids used
    std::set<TString> semanticNameSet;

    EShTextureSamplerTransformMode textureSamplerTransformMode;

    bool needToLegalize;
    bool binaryDoubleOutput;

private:
    void operator=(TIntermediate&); // prevent assignments
};

} // end namespace glslang

#endif // _LOCAL_INTERMEDIATE_INCLUDED_
