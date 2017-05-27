// Copyright Hugh Perkins 2016, 2017

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// For doc, please see the corresponding include file, patch_hostside.h

#include "patch_hostside.h"

#include "cocl_logging.h"

#include "mutations.h"
#include "struct_clone.h"
// #include "ir-to-opencl-common.h"
#include "argparsecpp.h"
#include "type_dumper.h"
#include "GlobalNames.h"
#include "EasyCL/util/easycl_stringhelper.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"

#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
// #include "llvm/IR/IRBuilder.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/DebugInfoMetadata.h"

#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_os_ostream.h"

#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <map>
#include <set>
#include <stdexcept>
#include <memory>
#include <sstream>
#include <fstream>

using namespace llvm;
using namespace std;
using namespace cocl;

namespace cocl {

static llvm::LLVMContext context;
// static std::string sourcecode_stringname;
static std::string devicellcode_stringname;
static string devicellfilename;

static GlobalNames globalNames;
static TypeDumper typeDumper(&globalNames);
static StructCloner structCloner(&typeDumper, &globalNames);

// static string deviceclfilename;
// static string clfilenamesimple;

// bool single_precision = true;

// this should probably be more of an instance variable probably?
static std::unique_ptr<LaunchCallInfo> launchCallInfo(new LaunchCallInfo);

std::unique_ptr<GenericCallInst> GenericCallInst::create(llvm::InvokeInst *inst) {
    return unique_ptr<GenericCallInst>(new GenericCallInst_Invoke(inst));
}
std::unique_ptr<GenericCallInst> GenericCallInst::create(llvm::CallInst *inst) {
    return unique_ptr<GenericCallInst>(new GenericCallInst_Call(inst));
}

std::ostream &operator<<(std::ostream &os, const LaunchCallInfo &info) {
    raw_os_ostream my_raw_os_ostream(os);
    my_raw_os_ostream << "LaunchCallInfo " << info.kernelName;
    my_raw_os_ostream << "<<<";

    my_raw_os_ostream << ">>>";
    my_raw_os_ostream << "(";
    int i = 0;
    // for(auto it=info.callTypes.begin(); it != info.callTypes.end(); it++) {
    for(auto it=info.params.begin(); it != info.params.end(); it++) {
        if(i > 0){
            my_raw_os_ostream << ", ";
        }
        const ParamInfo *paramInfo = it->get();
        // Type *type = *it;
        paramInfo->typeDevicesideFn->print(my_raw_os_ostream);
        i++;
    }
    my_raw_os_ostream << ");\n";
    my_raw_os_ostream << "value types: ";
    i = 0;
    for(auto it=info.params.begin(); it != info.params.end(); it++) {
    // for(auto it=info.callValuesByValue.begin(); it != info.callValuesByValue.end(); it++) {
        const ParamInfo *paramInfo = it->get();
        Value *value = paramInfo->value;
        // Value *value = *it;
        if(i > 0) {
            my_raw_os_ostream << ", ";
        }
        my_raw_os_ostream << typeDumper.dumpType(value->getType());
        i ++;
    }
    return os;
}

std::ostream &operator<<(std::ostream &os, const PointerInfo &pointerInfo) {
    os << "PointerInfo(offset=" << pointerInfo.offset << ", type=" << typeDumper.dumpType(pointerInfo.type);
    os << " indices=";
    int i = 0;
    for(auto it=pointerInfo.indices.begin(); it != pointerInfo.indices.end(); it++) {
        if(i > 0) {
            os << ",";
        }
        os << *it;
        i++;
    }
    os << ")";
    return os;
}

llvm::Instruction *PatchHostside::addSetKernelArgInst_int(llvm::Instruction *lastInst, llvm::Value *value, llvm::IntegerType *intType) {
    Module *M = lastInst->getModule();

    int bitLength = intType->getBitWidth();
    string mangledName = "";
    if(bitLength == 32) {
        mangledName = "setKernelArgInt32";
    } else if(bitLength == 64) {
        mangledName = "setKernelArgInt64";
    } else if(bitLength == 8) {
        mangledName = "setKernelArgInt8";
    } else {
        throw std::runtime_error("bitlength " + easycl::toString(bitLength) + " not implemented");
    }
    Function *setKernelArgInt = cast<Function>(M->getOrInsertFunction(
        mangledName,
        Type::getVoidTy(context),
        IntegerType::get(context, bitLength),
        NULL));
    CallInst *call = CallInst::Create(setKernelArgInt, value);
    call->insertAfter(lastInst);
    lastInst = call;
    return lastInst;
}

llvm::Instruction *PatchHostside::addSetKernelArgInst_float(llvm::Instruction *lastInst, llvm::Value *value) {
    // handle primitive floats, which we pass to `setKernelArgFloat`, by value

    Module *M = lastInst->getModule();

    Type *valueType = value->getType();
    // cout << "addSetKernelArgInst_float, value type:" << endl;
    // valueType->dump();
    // cout << endl;
    if(valueType->isDoubleTy()) {
        // cout << "is double" << endl;
        // FPTruncInst *fpTrunc = new FPTruncInst(value, Type::getFloatTy(context), "trunctofloat");
        // fpTrunc->insertAfter(lastInst);
        // lastInst = fpTrunc;
        // value = fpTrunc;
        cout << "Executing functions with doubles as kernel parameters is not supported" << endl;
        cout << "Note that this is not set-in-stone, but is really hard to do." << endl;
        cout << "If you really need this functionality, please post your use-case into https://github.com/hughperkins/coriander/issues/22" << endl;
        throw runtime_error("Executing functions with doubles as kernel parameters is not supported");
    }

    Function *setKernelArgFloat = cast<Function>(M->getOrInsertFunction(
        "setKernelArgFloat",
        Type::getVoidTy(context),
        Type::getFloatTy(context),
        NULL));
    CallInst *call = CallInst::Create(setKernelArgFloat, value);
    call->insertAfter(lastInst);
    return call;
}

llvm::Instruction *PatchHostside::addSetKernelArgInst_pointer(llvm::Instruction *lastInst, llvm::Value *value) {
    // cout << "lastInst:" << endl;
    // lastInst->dump();
    // cout << endl;
    // cout << "lastInst M " << lastInst->getModule() << endl;
    // cout << "value M " << value->getType()->getModule() << endl;

    Indentor indentor;
    Type *valueType = value->getType();
    // cout << "getting M" << endl;
    Module *M = lastInst->getModule();
    // cout << "M=" << M << endl;

    // value->dump();
    // cout << endl;

    Type *elementType = cast<PointerType>(valueType)->getPointerElementType();
    // cout << "addSetKernelArgInst_pointer elementType:" << endl;
    // elementType->dump();
    // we can probably generalize these to all just send as a pointer to char
    // we'll need to cast them somehow first
    // cout << "got elementtype" << endl;
    // elementType->dump();
    // cout << endl;

    if(elementType->isDoubleTy()) {
        // cout << "is double" << endl;
        // FPTruncInst *fpTrunc = new FPTruncInst(value, Type::getFloatTy(context), "trunctofloat");
        // fpTrunc->insertAfter(lastInst);
        // lastInst = fpTrunc;
        // value = fpTrunc;
        cout << "Executing functions with double arrays as kernel parameters is not supported" << endl;
        cout << "Note that this is not set-in-stone, but is really hard to do." << endl;
        cout << "If you really need this functionality, please post your use-case into https://github.com/hughperkins/coriander/issues/22" << endl;
        throw runtime_error("Executing functions with double arrays as kernel parameters is not supported");
    }

    BitCastInst *bitcast = new BitCastInst(value, PointerType::get(IntegerType::get(context, 8), 0));
    bitcast->insertAfter(lastInst);
    lastInst = bitcast;

    const DataLayout *dataLayout = &M->getDataLayout();
    int allocSize = dataLayout->getTypeAllocSize(elementType);
    // cout << "allocsize " << allocSize << endl;
    int32_t elementSize = allocSize;

    Function *setKernelArgGpuBuffer = cast<Function>(M->getOrInsertFunction(
        "setKernelArgGpuBuffer",
        Type::getVoidTy(context),
        PointerType::get(IntegerType::get(context, 8), 0),
        IntegerType::get(context, 32),
        NULL));
    Value *args[] = {bitcast, createInt32Constant(&context, elementSize)};
    CallInst *call = CallInst::Create(setKernelArgGpuBuffer, ArrayRef<Value *>(args));
    call->insertAfter(lastInst);
    lastInst = call;
    return lastInst;
}

llvm::Instruction *PatchHostside::addSetKernelArgInst_pointerstruct(llvm::Instruction *lastInst, llvm::Value *structPointer) {
    // what this will need to do is:
    // - create a call to pass the gpu buffer, that contains the struct, to hostside_opencl_funcs, at runtime
    // ~~- if the struct contains pointers, then add appropriate calls to pass those at runtime too~~
    // update: we're going to forbid gpuside buffers containing pointers, for now

    // lets deal with the gpuside buffer first, that should be fairly easy-ish:
    lastInst = addSetKernelArgInst_pointer(lastInst, structPointer);

    // we'd better at least assert or something, if there are pointers in the struct
    Module *M = lastInst->getModule();
    Type *valueType = structPointer->getType();
    StructType *structType = cast<StructType>(cast<PointerType>(valueType)->getPointerElementType());
    unique_ptr<StructInfo> structInfo(new StructInfo());
    StructCloner::walkStructType(M, structInfo.get(), 0, 0, vector<int>(), "", structType);
    bool structHasPointers = structInfo->pointerInfos.size() > 0;
    if(structHasPointers) {
        cout << "ERROR: Coriander currently forbids pointers inside gpu-side structs, passed into kernels" << endl;
        cout << "If you need this, please create an issue at https://github.com/hughperkins/Coriander/issues" << endl;
        cout << "Note that it's pretty hard to do though" << endl;
        structPointer->dump();
        cout << endl;
        throw std::runtime_error("ERROR: Coriander currently forbids pointers inside gpu-side structs, passed into kernels");
    }
    return lastInst;
}

llvm::Instruction *PatchHostside::addSetKernelArgInst_byvaluevector(llvm::Instruction *lastInst, llvm::Value *vectorPointer) {
    // so, lets assume its just a hostside blob, that we need to send deviceside, as a blob
    // we should thus find out the blob size
    // we'll need:
    // - element count
    // - element size

    Type *valueType = vectorPointer->getType();
    VectorType *vectorType = cast<VectorType>(cast<PointerType>(valueType)->getPointerElementType());
    // VectorType *vectorType = cast<VectorType>(valueType);
    // outs() << "vectorType:\n";
    // vectorType->dump();

    // VectorType *vectorType = cast<VectorType>(vectorPointer->getType());
    // vectorType->dump();
    // cout << endl;
    int elementCount = vectorType->getNumElements();
    Type *elementType = vectorType->getElementType();
    int elementSizeBytes = elementType->getPrimitiveSizeInBits() / 8;
    // cout << "PatchHostside::addSetKernelArgInst_byvaluevector count=" << elementCount <<
    //     " elementsizebytes=" << elementSizeBytes << " type=" <<  endl;
    // elementType->dump();
    // cout << endl;
    if(elementType->getPrimitiveSizeInBits() == 0) {
        cout << endl;
        vectorType->dump();
        cout << endl;
        throw runtime_error("PatchHostside::addSetKernelArgInst_byvaluevector: not implemented for non-primitive types");
    }

    int allocSize = elementSizeBytes * elementCount;

    Module *M = lastInst->getModule();

    BitCastInst *bitcast = new BitCastInst(vectorPointer, PointerType::get(IntegerType::get(context, 8), 0));
    bitcast->insertAfter(lastInst);
    lastInst = bitcast;

    Value *args[2];
    args[0] = bitcast;
    args[1] = createInt32Constant(&context, allocSize);

    Function *setKernelArgHostsideBuffer = cast<Function>(M->getOrInsertFunction(
        "setKernelArgHostsideBuffer",
        Type::getVoidTy(context),
        PointerType::get(IntegerType::get(context, 8), 0),
        IntegerType::get(context, 32),
        NULL));

    // cout << "adding setKernelArgHostsideBuffer call" << endl;
    CallInst *call = CallInst::Create(setKernelArgHostsideBuffer, ArrayRef<Value *>(args));
    call->insertAfter(lastInst);
    // call->dump();
    lastInst = call;

    return lastInst;
}

void addMetadata(Instruction *value, string tag) {
    // LLVMContext *context = value->getContext();
    MDNode* mdnode = MDNode::get(context, MDString::get(context, tag));
    value->setMetadata(tag, mdnode);
}

llvm::Instruction *PatchHostside::addSetKernelArgInst_byvaluestruct(llvm::Instruction *lastInst, ParamInfo *paramInfo, llvm::Value *structPointer) {
    //
    // what this is going to do is:
    // - create a call to pass the hostside buffer to hostside_opencl_funcs, at runtime
    // - if the struct contains pointers, then add appropriate calls to pass those at runtime too
    //

    Module *M = lastInst->getModule();

    // outs() << "got a byvalue struct" << "\n";

    Indentor indentor;
    PointerType *structPointerType = cast<PointerType>(structPointer->getType());
    // structPointerType->dump();

    indentor << "structPointerType is pointer? " << isa<PointerType>(structPointerType) << endl;
    indentor << "isa struct pointer? " << isa<StructType>(structPointer->getType()) << endl;

    if(!isa<StructType>(structPointerType->getPointerElementType())) {
        indentor << "doesnt actually point to a struct, currenlty, in hostside bytecode => adding a bitcast" << endl;
        // paramInfo->typeDevicesideFn
        // string deviceSideTypeName = paramInfo->typeDevicesideFn->getName().str():
        // indentor << "deviceSideTypeName: " << deviceSideTypeName << endl;
        paramInfo->typeDevicesideFn->dump();
        indentor << "dump type on this: " << typeDumper.dumpType(paramInfo->typeDevicesideFn) << endl;
        // BitCastInst *bitcast = new BitCastInst(structPointer, paramInfo->typeDevicesideFn, 0);
        // PointerType *pointerStruct = 
        BitCastInst *bitcast = new BitCastInst(structPointer, paramInfo->typeDevicesideFn);
        bitcast->insertAfter(lastInst);
        bitcast->dump();
        indentor << "after creating bitcast" << endl;
        structPointer = bitcast;
        lastInst = bitcast;

        structPointerType = cast<PointerType>(structPointer->getType());
        indentor << "structPointerType is pointer? " << isa<PointerType>(structPointerType) << endl;
        indentor << "isa struct pointer? " << isa<StructType>(structPointer->getType()) << endl;
    }

    indentor << "addSetKernelArgInst_byvaluestruct structPointerType->dump():" << endl;
    structPointerType->dump();
    indentor << "addSetKernelArgInst_byvaluestruct structPointerType->getPointerElementType()->dump():" << endl;
    structPointerType->getPointerElementType()->dump();

    StructType *structType = cast<StructType>(structPointerType->getPointerElementType());
    // outs() << "structType:\n";
    // structType->dump();
    string typeName = structType->getName().str();
    // cout << "typeName [" << typeName << "]" << endl;
    if(typeName == "struct.float4") {
        return PatchHostside::addSetKernelArgInst_pointer(lastInst, structPointer);
    }

    unique_ptr<StructInfo> structInfo(new StructInfo());
    StructCloner::walkStructType(M, structInfo.get(), 0, 0, vector<int>(), "", structType);
    bool structHasPointers = structInfo->pointerInfos.size() > 0;

    // StructType *structType = cast<StructType>(valueType);
    string name = globalNames.getOrCreateName(structType);
    Type *newType = structCloner.cloneNoPointers(structType);

    const DataLayout *dataLayout = &M->getDataLayout();
    int allocSize = dataLayout->getTypeAllocSize(newType);
    // outs() << "original typeallocsize " << dataLayout->getTypeAllocSize(value->getType()) << "\n";
    // outs() << "pointerfree typeallocsize " << allocSize << "\n";

    Value *sourceStruct = 0;
    if(structHasPointers) {
        Instruction *alloca = new AllocaInst(newType, "newalloca");
        alloca->insertAfter(lastInst);
        lastInst = alloca;

        // cout << "struct has pointers, so cloning" << endl;
        lastInst = structCloner.createHostsideIrCopyPtrfullToNoptr(lastInst, structType, structPointer, alloca);
        sourceStruct = alloca;
    } else {
        // cout << "struct does not have pointers, no need to clone" << endl;
        sourceStruct = structPointer;
    }

    BitCastInst *bitcast = new BitCastInst(sourceStruct, PointerType::get(IntegerType::get(context, 8), 0));
    bitcast->insertAfter(lastInst);
    lastInst = bitcast;

    Value *args[2];
    args[0] = bitcast;
    args[1] = createInt32Constant(&context, allocSize);

    Function *setKernelArgHostsideBuffer = cast<Function>(M->getOrInsertFunction(
        "setKernelArgHostsideBuffer",
        Type::getVoidTy(context),
        PointerType::get(IntegerType::get(context, 8), 0),
        IntegerType::get(context, 32),
        NULL));

    // cout << "adding setKernelArgHostsideBuffer call" << endl;
    CallInst *call = CallInst::Create(setKernelArgHostsideBuffer, ArrayRef<Value *>(args));
    call->insertAfter(lastInst);
    // call->dump();
    lastInst = call;

    // now we have to handle any pointers, send those through too

    // outs() << "pointers in struct:" << "\n";
    int i = 0;
    for(auto pointerit=structInfo->pointerInfos.begin(); pointerit != structInfo->pointerInfos.end(); pointerit++) {
        // cout << "   passing a pointer, from the struct" << endl;
        PointerInfo *pointerInfo = pointerit->get();
        vector<Value *> indices;
        indices.push_back(createInt32Constant(&context, 0));
        for(auto idxit = pointerInfo->indices.begin(); idxit != pointerInfo->indices.end(); idxit++) {
            int idx = *idxit;
            // outs() << "idx " << idx << "\n";
            indices.push_back(createInt32Constant(&context, idx));
        }
        GetElementPtrInst *gep = GetElementPtrInst::CreateInBounds(structType, structPointer, ArrayRef<Value *>(&indices[0], &indices[indices.size()]), "getfloatstaraddr");
        gep->insertAfter(lastInst);
        lastInst = gep;

        LoadInst *loadgep = new LoadInst(gep, "loadgep");
        loadgep->insertAfter(lastInst);
        lastInst = loadgep;

        lastInst = PatchHostside::addSetKernelArgInst_pointer(lastInst, loadgep);
        i++;
    }
    return lastInst;
}

llvm::Instruction *PatchHostside::addSetKernelArgInst(llvm::Instruction *lastInst, ParamInfo *paramInfo) {
    /*
    This figures out the type of the argument, and dispatches appropriately. All arguments pass through this function,
    including primitives, pointers, structs, ...
    */
    // Type *typeHostsideFn = paramInfo->typeHostsideFn;
    Value *value = paramInfo->value;
    Value *valueAsPointerInstr = paramInfo->pointer;

    Indentor indentor_;
    indentor_ << "addSetKErnelArgInst arg=" << paramInfo->paramIndex << endl;

    Indentor indentor;

    // int size = paramInfo->size;
    // bool definitelyNotAPointer = paramInfo->size != 4 && paramInfo->size != 8;
    // bool couldBePrimitiveInt = paramInfo->size == 4 || paramInfo->size == 8;
    // bool couldBePrimitiveFloat = paramInfo->size == 4 || paramInfo->size == 8;
    // bool couldBePointer = paramInfo->size == 4 || paramInfo->size == 8;
    // bool clearlyAByValueStruct = paramInfo->size > 8;
    // bool definitelyNotAPointer = false;
    // indentor << "size=" << paramInfo->size << endl;

    Type *devicesideType = paramInfo->typeDevicesideFn;

    if(paramInfo->devicesideByVal &&
            isa<PointerType>(devicesideType) &&
            isa<StructType>(devicesideType->getPointerElementType())) {
        indentor << "deviceside is by value struct, so we will pass by value, as a struct" << endl;
        // indentor << "clearly a by value struct" << endl;
        // value->dump();
        // valueAsPointerInstr->dump();
        // paramInfo->type->dump();

        // lets walk up the gep???
        if(GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(valueAsPointerInstr)) {
            // indentor << "got a gep" << endl;
            // indentor << "valueAsPointer is gep? " << isa<GetElementPtrInst>(valueAsPointerInstr) << endl;
            Value *beforeGep = gep->getOperand(0);
            // beforeGep->dump();
            valueAsPointerInstr = beforeGep;
        }

        // lastInst = PatchHostside::addSetKernelArgInst_byvaluestruct(lastInst, value);
        lastInst = PatchHostside::addSetKernelArgInst_byvaluestruct(lastInst, paramInfo, valueAsPointerInstr);
    } else if(IntegerType *intType = dyn_cast<IntegerType>(value->getType())) {
        indentor << "addSetKernelArgInst: int primitive" << endl;
        lastInst = PatchHostside::addSetKernelArgInst_int(lastInst, value, intType);
    } else if(value->getType()->isFloatingPointTy()) {
        indentor << "addSetKernelArgInst: float primitive" << endl;
        lastInst = PatchHostside::addSetKernelArgInst_float(lastInst, value);
    } else if(value->getType()->isPointerTy()) {
        // cout << "pointer" << endl;
        Type *elementType = dyn_cast<PointerType>(value->getType())->getPointerElementType();
        if(isa<StructType>(elementType)) {
            // lastInst = PatchHostside::addSetKernelArgInst_pointer(lastInst, value);
            // indentor << "pointer to struct" << endl;
            // lastInst = addSetKernelArgInst_byvaluestruct(lastInst, value);
            indentor << "addSetKernelArgInst: pointerstruct" << endl;
            lastInst = PatchHostside::addSetKernelArgInst_pointerstruct(lastInst, value);
        } else {
            indentor << "addSetKernelArgInst: pointer" << endl;
            lastInst = PatchHostside::addSetKernelArgInst_pointer(lastInst, value);
        }
    } else if(isa<StructType>(value->getType())) {
        // cout << "structtype" << endl;
        indentor << "addSetKernelArgInst: byvalue struct" << endl;
        lastInst = PatchHostside::addSetKernelArgInst_byvaluestruct(lastInst, paramInfo, valueAsPointerInstr);
    } else if(isa<VectorType>(value->getType())) {
        // cout << "got vector arg type" << endl;
        // value->dump();
        // cout << endl;
        // value->getType()->dump();
        // cout << endl;
        // so, this is byval, thus hostside?
        // so send it as a hostside buffer?
        indentor << "addSetKernelArgInst: byvalue vector" << endl;
        lastInst = PatchHostside::addSetKernelArgInst_byvaluevector(lastInst, valueAsPointerInstr);
    } else {
        value->dump();
        outs() << "\n";
        throw std::runtime_error("kernel arg type type not implemented " + typeDumper.dumpType(value->getType()));
    }
    return lastInst;
}

void PatchHostside::getLaunchArgValue(GenericCallInst *inst, LaunchCallInfo *info, ParamInfo *paramInfo) {
    // input to this is:
    // - inst is cudaSetupArgument instruction, with:
    //   - first operand is a value pointing to the value we want to send to the kernel
    //
    // - output of this method is
    //    populate info with a Value holding the actual concrete value w ewant to send to the kernel
    //    (note a pointer to it, since we Load the pointer)
    // Notes:
    // - the first operand of inst was created as bitcast(i8*)(alloca (type-of-arg))
    // - the alloca instruction is inst->getOperand(0)->getOperand(0)
    // - so if we load from the alloca instruction, we should have the value we want?
    // outs() << "getLaunchArgValue " << "\n";
    Indentor indentor;
    int size = (int)cast<ConstantInt>(inst->getOperand(1))->getZExtValue();
    paramInfo->size = size;
    if(!isa<Instruction>(inst->getOperand(0))) {
        outs() << "getlaunchvalue, first operatnd of inst is not an instruction..." << "\n";
        inst->dump();
        outs() << "\n";
        inst->getOperand(0)->dump();
        outs() << "\n";
        throw runtime_error("getlaunchvalue, first operatnd of inst is not an instruction...");
    }
    Instruction *bitcast = cast<Instruction>(inst->getOperand(0));
    Value *alloca = bitcast;
    if(isa<BitCastInst>(bitcast)) {
        alloca = bitcast->getOperand(0);
    } else {
        alloca = bitcast;
    }
    Instruction *load = new LoadInst(alloca, "loadCudaArg");
    load->insertBefore(inst->getInst());
    paramInfo->value = load;
    paramInfo->pointer = alloca;
    // info->callValuesByValue.push_back(load);
    // info->callValuesAsPointers.push_back(alloca);
}

void PatchHostside::getLaunchTypes(
        llvm::Module *M, const llvm::Module *MDevice, GenericCallInst *inst, LaunchCallInfo *info) {
    // input to this is a cudaLaunch instruction
    // sideeffect is to populate in info:
    // - name of the kernel
    // - type of each of the kernel parameters (without the actual Value's)
    // info->callTypes.clear();
    // outs() << "getLaunchTypes()\n";

    Indentor indentor;

    // the cudaLaunch bitcasts the function to a char *:
    // so we need to walk back along that to get the original function:
    BitCastInst *bitcast = cast<BitCastInst>(cast<ConstantExpr>(inst->getArgOperand(0))->getAsInstruction());
    Function *hostFn = cast<Function>(bitcast->getOperand(0));

    PointerType *pointerFunctionType = cast<PointerType>(hostFn->getType());
    FunctionType *hostFnType = cast<FunctionType>(pointerFunctionType->getPointerElementType());

    info->kernelName = hostFn->getName();

    Function *deviceFn = MDevice->getFunction(info->kernelName);
    if(deviceFn != 0) {
        indentor << "Got device function:" << endl;
        // deviceFn->dump();
    } else {
        cout << "ERROR: failed to find device kernel [" << info->kernelName << "]" << endl;
        throw runtime_error("ERROR: failed to find device kernel " + info->kernelName);
    }
    FunctionType *deviceFnType = cast<FunctionType>(deviceFn->getType()->getPointerElementType());

    int i = 0;
    // vector<Argument *> hostsideArgs(hostFn->arg_begin(), hostFn->arg_end());
    // vector<Argument *> hostsideArgs;
    // for(auto it=hostFn->arg_begin(); it != hostFn->arg_end(); it++) {
    //     hostsideArgs.push_back(&*it);
    // }
    vector<Argument *> devicesideArgs;
    for(auto it=deviceFn->arg_begin(); it != deviceFn->arg_end(); it++) {
        Argument *arg = &*it;
        // bool readNone = arg->hasAttribute(Attribute::ReadNone);
        // cout << "device side arg hasAttr readnone? " << readNone << endl;
        // if(!readNone) {
            devicesideArgs.push_back(&*it);
        // }
    }
    // for(auto it=hostFnType->param_begin(); it != hostFnType->param_end(); it++) {
    for(auto it=deviceFnType->param_begin(); it != deviceFnType->param_end(); it++) {
        Type * typeDevicesideFn = *it;
        if(i >= (int)info->params.size()) {
            cout << "warning: exceeded number of params" << endl;
            break;
        }
        indentor << "params[" << i << "]" << endl;
        {
            Indentor indentor;
            ParamInfo *paramInfo = info->params[i].get();

            paramInfo->paramIndex = i;

            // paramInfo->hostsideArg = hostsideArgs[i];
            paramInfo->devicesideArg = devicesideArgs[i];
            // paramInfo->hostsideByVal = paramInfo->hostsideArg->hasByValAttr();
            paramInfo->devicesideByVal = paramInfo->devicesideArg->hasByValAttr();

            // indentor << "hostside byval " << paramInfo->hostsideArg->hasByValAttr() << endl;
            indentor << "deviceside byval " << paramInfo->devicesideArg->hasByValAttr() << endl;
            // indentor << "hostsidearg" << endl;
            // hostsideArg->dump();
            // indentor << "devicesideArg" << endl;
            // devicesideArg->dump();

            // paramInfo->typeHostsideFn = typeHostsideFn;
            // paramInfo->typeDevicesideFn = deviceFnType->getParamType(i);
            paramInfo->typeDevicesideFn = typeDevicesideFn;
            // indentor << "hostside type " << typeDumper.dumpType(paramInfo->typeHostsideFn) << endl;
            indentor << "deviceside type " << typeDumper.dumpType(paramInfo->typeDevicesideFn) << endl;
        }
        i++;
    }
}

void PatchHostside::patchCudaLaunch(
        llvm::Module *M, const llvm::Module *MDevice, llvm::Function *F,
        GenericCallInst *inst, std::vector<llvm::Instruction *> &to_replace_with_zero) {
    // This replaces the call to cudaLaunch with calls to setup the arguments, then
    // a call to kernelGo
    // We pass the informatino about the kernel first, then the arguments, then finally
    // call kernelGo.  This is slightly different than the original NVIDIA sequence, which
    // doenst pass the kernel function name etc until the call to cudaLaunch

    // MDevice is just so we can see how the device-side kernel declarations look, we dont
    // modify/patch it in any way here

    // outs() << "============\n";
    // outs() << "cudaLaunch\n";

    PatchHostside::getLaunchTypes(M, MDevice, inst, launchCallInfo.get());
    to_replace_with_zero.push_back(inst->getInst());

    string kernelName = launchCallInfo->kernelName;
    Instruction *kernelNameValue = addStringInstr(M, "s_" + ::devicellcode_stringname + "_" + kernelName, kernelName);
    kernelNameValue->insertBefore(inst->getInst());

    // this isnt actually needed for running, but hopefully useful for debugging
    Instruction *llSourcecodeValue = addStringInstrExistingGlobal(M, devicellcode_stringname);
    llSourcecodeValue->insertBefore(inst->getInst());

    Function *configureKernel = cast<Function>(F->getParent()->getOrInsertFunction(
        "configureKernel",
        Type::getVoidTy(context),
        PointerType::get(IntegerType::get(context, 8), 0),
        PointerType::get(IntegerType::get(context, 8), 0),
        // PointerType::get(IntegerType::get(context, 8), 0),
        NULL));
    Value *args[] = {kernelNameValue, llSourcecodeValue};
    CallInst *callConfigureKernel = CallInst::Create(configureKernel, ArrayRef<Value *>(&args[0], &args[2]));
    callConfigureKernel->insertBefore(inst->getInst());
    Instruction *lastInst = callConfigureKernel;

    // pass args now
    int i = 0;
    for(auto argit=launchCallInfo->params.begin(); argit != launchCallInfo->params.end(); argit++) {
        ParamInfo *paramInfo = argit->get();
        lastInst = PatchHostside::addSetKernelArgInst(lastInst, paramInfo);
        i++;
    }
    // trigger the kernel...
    Function *kernelGo = cast<Function>(F->getParent()->getOrInsertFunction(
        "kernelGo",
        Type::getVoidTy(context),
        NULL));
    CallInst *kernelGoInst = CallInst::Create(kernelGo);
    kernelGoInst->insertAfter(lastInst);
    lastInst = kernelGoInst;

    launchCallInfo->params.clear();
}

void PatchHostside::patchFunction(llvm::Module *M, const llvm::Module *MDevice, llvm::Function *F) {
    // this will take the calls to cudaSetupArgument(someArg, argSize, ...), and
    // cudaLaunch(function), and rewrite them to call Coriander instead
    // we do a bunch of bytecode parsing, to figure out how exactly we are goign to get the arguments
    // into coriander
    // For example, if it's a by-value struct, we're going to have to do some hacking
    // If the by-value struct contains pointers, it'll need a bit (lot :-P) more hacking

    // MDevice is only for information, so we can see the declaration of kernels on the device-side

    cout << "=========================" << endl;
    bool is_main = (string(F->getName().str()) == "main");
    if(is_main) cout << "patching " << F->getName().str() << endl;    
    cout << "patching " << F->getName().str() << endl;    
    vector<Instruction *> to_replace_with_zero;
    IntegerType *inttype = IntegerType::get(context, 32);
    ConstantInt *constzero = ConstantInt::getSigned(inttype, 0);
    launchCallInfo->params.clear();
    Indentor indentor;
    for(auto it=F->begin(); it != F->end(); it++) {
        BasicBlock *basicBlock = &*it;
        for(auto insit=basicBlock->begin(); insit != basicBlock->end(); insit++) {
            Instruction *inst = &*insit;
            if(!isa<CallInst>(inst) && !isa<InvokeInst>(inst)) {
                continue;
            }
            Function *called = 0;
            unique_ptr<GenericCallInst> genCallInst;
            if(CallInst *callInst = dyn_cast<CallInst>(inst)) {
                called = callInst->getCalledFunction();
                genCallInst = GenericCallInst::create(callInst);
            } else if(InvokeInst *callInst = dyn_cast<InvokeInst>(inst)) {
                called = callInst->getCalledFunction();
                genCallInst = GenericCallInst::create(callInst);
            }
            if(called == 0) {
                continue;
            }
            if(!called->hasName()) {
                continue;
            }
            string calledFunctionName = called->getName();
            // if(is_main && calledFunctionName.find("cuda") != string::npos) cout << "calledfunctionname " << calledFunctionName << endl;
            if(calledFunctionName == "cudaSetupArgument") {
                unique_ptr<ParamInfo> paramInfo(new ParamInfo());
                PatchHostside::getLaunchArgValue(genCallInst.get(), launchCallInfo.get(), paramInfo.get());
                launchCallInfo->params.push_back(std::move(paramInfo));
                // indentor << " creating paraminfo params size " << launchCallInfo->params.size() << endl;
                to_replace_with_zero.push_back(inst);
            } else if(calledFunctionName == "cudaLaunch") {
                PatchHostside::patchCudaLaunch(M, MDevice, F, genCallInst.get(), to_replace_with_zero);
            }
        }
    }
    for(auto it=to_replace_with_zero.begin(); it != to_replace_with_zero.end(); it++) {
        Instruction *inst = *it;
        BasicBlock::iterator ii(inst);
        if(InvokeInst *invoke = dyn_cast<InvokeInst>(inst)) {
            // need to add an uncondtioinal branch, after the old invoke locaiton
            // cout << "replacing an invoke, need to patch in a branch" << endl;
            BasicBlock *oldTarget = invoke->getNormalDest();
            BranchInst *branch = BranchInst::Create(oldTarget);
            branch->insertAfter(inst);
        }
        ReplaceInstWithValue(inst->getParent()->getInstList(), ii, constzero);
    }
}

std::string PatchHostside::getBasename(std::string path) {
    // grab anything after final / ,or whole string
    size_t slash_pos = path.rfind('/');
    if(slash_pos == string::npos) {
        return path;
    }
    return path.substr(slash_pos + 1);
}

void PatchHostside::patchModule(Module *M, const Module *MDevice) {
    // entry point: given Module M, traverse all functions, rewriting the launch instructison to call
    // into Coriander runtime

    // MDevice is only for information, so we can see the declaration of kernels on the device-side

    ifstream f_inll(::devicellfilename);
    string devicell_sourcecode(
        (std::istreambuf_iterator<char>(f_inll)),
        (std::istreambuf_iterator<char>()));

    ::devicellcode_stringname = "__devicell_sourcecode" + ::devicellfilename;
    addGlobalVariable(M, devicellcode_stringname, devicell_sourcecode);

    for(auto it = M->begin(); it != M->end(); it++) {
        Function *F = &*it;
        PatchHostside::patchFunction(M, MDevice, F);
        verifyFunction(*F);
    }
    // cout << "after loop" << endl;
}

} // namespace cocl

int main(int argc, char *argv[]) {
    SMDiagnostic smDiagnostic;
    argparsecpp::ArgumentParser parser;

    string rawhostfilename;
    string patchedhostfilename;

    parser.add_string_argument("--hostrawfile", &rawhostfilename)->required()->help("input file");
    parser.add_string_argument("--devicellfile", &::devicellfilename)->required()->help("input file");
    parser.add_string_argument("--hostpatchedfile", &patchedhostfilename)->required()->help("output file");
    if(!parser.parse_args(argc, argv)) {
        return -1;
    }

    std::unique_ptr<llvm::Module> module = parseIRFile(rawhostfilename, smDiagnostic, context);
    if(!module) {
        smDiagnostic.print(argv[0], errs());
        return 1;
    }
    std::unique_ptr<llvm::Module> deviceModule = parseIRFile(devicellfilename, smDiagnostic, context);
    if(!deviceModule) {
        smDiagnostic.print(argv[0], errs());
        return 1;
    }

    try {
        PatchHostside::patchModule(module.get(), deviceModule.get());
    } catch(const runtime_error &e) {
        cout << endl;
        cout << "Something went wrong, sorry." << endl;
        cout << endl;
        cout << "More detail for devs/maintainers:" << endl;
        cout << "  exception: " << e.what() << endl;
        cout << "  rawhost ll file: " << rawhostfilename << "\n";
        outs() << "  hostpatched file: " << patchedhostfilename << "\n";
        cout << endl;
        return -1;
    }

    AssemblyAnnotationWriter assemblyAnnotationWriter;
    ofstream ofile;
    ofile.open(patchedhostfilename);
    raw_os_ostream my_raw_os_ostream(ofile);
    verifyModule(*module);
    module->print(my_raw_os_ostream, &assemblyAnnotationWriter);
    ofile.close();
    return 0;
}
