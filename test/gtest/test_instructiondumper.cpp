// Copyright Hugh Perkins 2016

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "InstructionDumper.h"

#include "type_dumper.h"
#include "GlobalNames.h"
#include "LocalNames.h"

#include "llvm/IRReader/IRReader.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <iostream>
#include <memory>

#include "gtest/gtest.h"

using namespace std;
using namespace cocl;
using namespace llvm;

namespace {

class StandaloneBlock{
public:
    StandaloneBlock() {
        M.reset(new Module("mymodule", context));
        F = cast<Function>(M->getOrInsertFunction(
            "mykernel",
            Type::getVoidTy(context),
            NULL
        ));
        F->setCallingConv(CallingConv::C);
        F->dump();
        block = BasicBlock::Create(context, "entry", F);
        block->dump();
    }
    virtual ~StandaloneBlock() {

    }
    LLVMContext context;
    unique_ptr<Module> M;
    Function *F;
    BasicBlock *block;
    // unique_ptrIRBuilder builder;
};

class InstructionDumperWrapper {
public:
    InstructionDumperWrapper() {
        typeDumper.reset(new TypeDumper(&globalNames));
        instructionDumper.reset(new InstructionDumper(&globalNames, &localNames, typeDumper.get(), &functionNamesMap,
            &allocaDeclarations, &variablesToDeclare, &sharedVariablesToDeclare, &shimFunctionsNeeded,
            &neededFunctions,
            &globalExpressionByValue, &localExpressionByValue));
    }
    virtual ~InstructionDumperWrapper() {

    }
    void runRhsGeneration(Instruction *inst) {
        instructionDumper->runRhsGeneration(inst, &extraInstructions, dumpedFunctions, returnTypeByFunction);
    }
    string getExpr(Instruction *inst) {
        return instructionDumper->localExpressionByValue->operator[](inst);
    }

    GlobalNames globalNames;
    LocalNames localNames;
    unique_ptr<TypeDumper> typeDumper;
    FunctionNamesMap functionNamesMap;

    set<Function *> dumpedFunctions;
    map<Function *, Type*> returnTypeByFunction;

    std::vector<AllocaInfo> allocaDeclarations;
    std::set<llvm::Value *> variablesToDeclare;
    std::set<llvm::Value *> sharedVariablesToDeclare;
    std::set<std::string> shimFunctionsNeeded;
    std::set<llvm::Function *> neededFunctions;

    std::map<llvm::Value *, std::string> globalExpressionByValue;
    std::map<llvm::Value *, std::string> localExpressionByValue;

    vector<string> extraInstructions;

    unique_ptr<InstructionDumper> instructionDumper;
};

// Lets think about how instructiondumper should behave for various inputs:
//
// we give it an add, for two declared values, it should output eg 'v1 + v2'
TEST(test_instructiondumper, add_two_declared_variables) {
    LLVMContext context;
    unique_ptr<Module>M(new Module("module", context));

    // we should create allocas really, and load those.  I guess?
    AllocaInst *a = new AllocaInst(IntegerType::get(context, 32));
    AllocaInst *b = new AllocaInst(IntegerType::get(context, 32));

    LoadInst *aLoad = new LoadInst(a);
    LoadInst *bLoad = new LoadInst(b);

    InstructionDumperWrapper wrapper;

    // since they are declared, we expect to find them in localnames:
    wrapper.localNames.getOrCreateName(aLoad, "v_a");
    wrapper.localNames.getOrCreateName(bLoad, "v_b");

    Instruction *add = BinaryOperator::Create(Instruction::FAdd, aLoad, bLoad);

    wrapper.runRhsGeneration(add);
    string expr = wrapper.getExpr(add);
    cout << "expr " << expr << endl;
    ASSERT_EQ("v_a + v_b", expr);

    // if we check local names, we should NOT find the add, since we havent declared it
    ASSERT_FALSE(wrapper.localNames.hasValue(add));

    // but we should find an expression for it:
    /// oh ... we already tested this :-)

    // we should not find a requirement to declare the variable
    ASSERT_EQ(0u, wrapper.variablesToDeclare.size());
    // ... and no allocas
    ASSERT_EQ(0u, wrapper.allocaDeclarations.size());
}

TEST(test_instructiondumper, add_two_declared_variables_using_builder) {
    StandaloneBlock myblock;
    IRBuilder<> builder(myblock.block);
    // LLVMContext context;
    // unique_ptr<Module>M(new Module("module", context));

    // we should create allocas really, and load those.  I guess?
    // AllocaInst *a = new AllocaInst(IntegerType::get(context, 32));
    // AllocaInst *b = new AllocaInst(IntegerType::get(context, 32));

    LLVMContext &context = myblock.context;
    AllocaInst *a = builder.CreateAlloca(IntegerType::get(context, 32));
    AllocaInst *b = builder.CreateAlloca(IntegerType::get(context, 32));

    // LoadInst *aLoad = new LoadInst(a);
    // LoadInst *bLoad = new LoadInst(b);

    LoadInst *aLoad = builder.CreateLoad(a);
    LoadInst *bLoad = builder.CreateLoad(b);

    InstructionDumperWrapper wrapper;

    // since they are declared, we expect to find them in localnames:
    wrapper.localNames.getOrCreateName(aLoad, "v_a");
    wrapper.localNames.getOrCreateName(bLoad, "v_b");

    // Instruction *add = BinaryOperator::Create(Instruction::FAdd, aLoad, bLoad);
    Instruction *add = cast<Instruction>(builder.CreateAdd(aLoad, bLoad));

    wrapper.runRhsGeneration(add);
    string expr = wrapper.getExpr(add);
    cout << "expr " << expr << endl;
    ASSERT_EQ("v_a + v_b", expr);

    // if we check local names, we should NOT find the add, since we havent declared it
    ASSERT_FALSE(wrapper.localNames.hasValue(add));

    // but we should find an expression for it:
    /// oh ... we already tested this :-)

    // we should not find a requirement to declare the variable
    ASSERT_EQ(0u, wrapper.variablesToDeclare.size());
    // ... and no allocas
    ASSERT_EQ(0u, wrapper.allocaDeclarations.size());
}

TEST(test_instructiondumper, callsomething) {
    StandaloneBlock myblock;
    IRBuilder<> builder(myblock.block);
    LLVMContext &context = myblock.context;
    Module *M = myblock.M.get();

    AllocaInst *charArray = builder.CreateAlloca(IntegerType::get(context, 8));
    cout << "charArray:" << endl;
    charArray->dump();
    cout << endl;

    Function *childF = cast<Function>(M->getOrInsertFunction(
        "mychildfunc",
        PointerType::get(IntegerType::get(context, 8), 0),
        PointerType::get(IntegerType::get(context, 8), 0),
        NULL));
    cout << "childF:" << endl;
    childF->dump();
    cout << endl;

    Value *args[] = {charArray};
    CallInst *call = builder.CreateCall(childF, ArrayRef<Value *>(args));
    cout << "call:" << endl;
    call->dump();
    cout << endl;

    InstructionDumperWrapper wrapper;
    wrapper.localNames.getOrCreateName(charArray, "myCharArray");
    wrapper.runRhsGeneration(call);
    string expr = wrapper.getExpr(call);
    cout << "expr " << expr << endl;
    ASSERT_EQ("", expr);

    InstructionDumper *instructionDumper = wrapper.instructionDumper.get();
    ASSERT_TRUE(instructionDumper->needDependencies);
    ASSERT_EQ(1u, instructionDumper->neededFunctions->size());
    (*instructionDumper->neededFunctions->begin())->dump();
    cout << endl;
    ASSERT_EQ(childF, (*instructionDumper->neededFunctions->begin()));
    ASSERT_EQ(0u, instructionDumper->generatedCl.size());
    ASSERT_EQ(0u, instructionDumper->variablesToDeclare->size());
    ASSERT_EQ(0u, instructionDumper->globalExpressionByValue->size());
    ASSERT_EQ(1u, instructionDumper->localExpressionByValue->size());
    auto it = instructionDumper->localExpressionByValue->begin();
    cout << "local expr 0 value " << endl;
    it->first->dump();
    cout << endl;
    ASSERT_EQ(call, it->first);
    cout << "local expr 0 name [" << it->second << "]" << endl;
    // ASSERT_EQ(call, it->first);
    // cout << instructionDumper->localExpressionByValue->operator[](0) << endl;

    // LLVMContext context;
    // unique_ptr<Module>M(new Module("module", context));
    // BasicBlock *block = 
    // Value *charArray = new AllocaInst(IntegerType::get(context, 8));
    // cout << "charArray:" << endl;
    // charArray->dump();
    // cout << endl;
    // Value *args[] = {charArray};
    // CallInst *call = CallInst::Create(childF, ArrayRef<Value *>(args));
    // cout << "call:" << endl;
    // call->dump();
    // cout << endl;
    // InstructionDumperWrapper wrapper;
    // wrapper.localNames.getOrCreateName(charArray, "myCharArray");
    // wrapper.runRhsGeneration(call);
    // string expr = wrapper.getExpr(call);
    // cout << "expr " << expr << endl;
}

TEST(test_instructiondumper, basic) {
    LLVMContext context;
    unique_ptr<Module>M(new Module("module", context));

    Value *a = ConstantInt::getSigned(IntegerType::get(context, 32), 123);
    Value *b = ConstantInt::getSigned(IntegerType::get(context, 32), 47);
    Instruction *add = BinaryOperator::Create(Instruction::FAdd, a, b);
    GlobalNames globalNames;
    LocalNames localNames;
    TypeDumper typeDumper(&globalNames);
    FunctionNamesMap functionNamesMap;

    std::vector<AllocaInfo> allocaDeclarations;
    std::set<llvm::Value *> variablesToDeclare;
    std::set<llvm::Value *> sharedVariablesToDeclare;
    std::set<std::string> shimFunctionsNeeded; // for __shfldown_3 etc, that we provide as opencl directly
    std::set<llvm::Function *> neededFunctions;

    std::map<llvm::Value *, std::string> globalExpressionByValue;
    std::map<llvm::Value *, std::string> localExpressionByValue;

    InstructionDumper instructionDumper(&globalNames, &localNames, &typeDumper, &functionNamesMap,
        &allocaDeclarations, &variablesToDeclare, &sharedVariablesToDeclare, &shimFunctionsNeeded,
        &neededFunctions,
        &globalExpressionByValue, &localExpressionByValue);
    vector<string> extraInstructions;
    std::set< llvm::Function *> dumpedFunctions;
    map<Function *, Type *>returnTypeByFunction;
    instructionDumper.runRhsGeneration(add, &extraInstructions, dumpedFunctions, returnTypeByFunction);
    string expr = instructionDumper.localExpressionByValue->operator[](add);

    cout << "expr " << expr << endl;
    ASSERT_EQ("123 + 47", expr);

    instructionDumper.localExpressionByValue->operator[](a) = "v1";
    instructionDumper.runRhsGeneration(add, &extraInstructions, dumpedFunctions, returnTypeByFunction);
    expr = instructionDumper.localExpressionByValue->operator[](add);
    cout << "expr " << expr << endl;

    a = new AllocaInst(IntegerType::get(context, 32));
    b = new AllocaInst(IntegerType::get(context, 32));
    instructionDumper.localExpressionByValue->operator[](a) = "v3";
    instructionDumper.localExpressionByValue->operator[](b) = "v4";
    add = BinaryOperator::Create(Instruction::Add, a, b);
    instructionDumper.runRhsGeneration(add, &extraInstructions, dumpedFunctions, returnTypeByFunction);
    expr = instructionDumper.localExpressionByValue->operator[](add);
    cout << "expr " << expr << endl;

    instructionDumper.localExpressionByValue->operator[](add) = "v5";
    // expr = instructionDumper.dumpInstructionRhs(add, &extraInstructions);
    instructionDumper.runRhsGeneration(add, &extraInstructions, dumpedFunctions, returnTypeByFunction);
    expr = instructionDumper.localExpressionByValue->operator[](add);
    cout << "expr " << expr << endl;
}

TEST(test_instructiondumper, globalexpr) {
    LLVMContext context;
    unique_ptr<Module>M(new Module("module", context));

    Value *a = ConstantInt::getSigned(IntegerType::get(context, 32), 123);
    Value *b = ConstantInt::getSigned(IntegerType::get(context, 32), 47);
    Instruction *add = BinaryOperator::Create(Instruction::FAdd, a, b);
    GlobalNames globalNames;
    LocalNames localNames;
    TypeDumper typeDumper(&globalNames);
    FunctionNamesMap functionNamesMap;

    std::vector<AllocaInfo> allocaDeclarations;
    std::set<llvm::Value *> variablesToDeclare;
    std::set<llvm::Value *> sharedVariablesToDeclare;
    std::set<std::string> shimFunctionsNeeded; // for __shfldown_3 etc, that we provide as opencl directly
    std::set<llvm::Function *> neededFunctions;

    std::map<llvm::Value *, std::string> globalExpressionByValue;
    std::map<llvm::Value *, std::string> localExpressionByValue;

    InstructionDumper instructionDumper(&globalNames, &localNames, &typeDumper, &functionNamesMap,
        &allocaDeclarations, &variablesToDeclare, &sharedVariablesToDeclare, &shimFunctionsNeeded,
        &neededFunctions,
        &globalExpressionByValue, &localExpressionByValue);
    vector<string> extraInstructions;
    std::set< llvm::Function *> dumpedFunctions;
    map<Function *, Type *>returnTypeByFunction;
    instructionDumper.runRhsGeneration(add, &extraInstructions, dumpedFunctions, returnTypeByFunction);
    string expr = instructionDumper.localExpressionByValue->operator[](add);
    cout << "expr " << expr << endl;
}

TEST(test_instructiondumper, alloca) {
    LLVMContext context;
    unique_ptr<Module>M(new Module("module", context));

    // Value *a = ConstantInt::getSigned(IntegerType::get(context, 32), 123);
    // Value *b = ConstantInt::getSigned(IntegerType::get(context, 32), 47);
    // Instruction *add = BinaryOperator::Create(Instruction::FAdd, a, b);
    GlobalNames globalNames;
    LocalNames localNames;
    TypeDumper typeDumper(&globalNames);
    FunctionNamesMap functionNamesMap;

    std::vector<AllocaInfo> allocaDeclarations;
    std::set<llvm::Value *> variablesToDeclare;
    std::set<llvm::Value *> sharedVariablesToDeclare;
    std::set<std::string> shimFunctionsNeeded; // for __shfldown_3 etc, that we provide as opencl directly
    std::set<llvm::Function *> neededFunctions;

    std::map<llvm::Value *, std::string> globalExpressionByValue;
    std::map<llvm::Value *, std::string> localExpressionByValue;

    InstructionDumper instructionDumper(&globalNames, &localNames, &typeDumper, &functionNamesMap,
        &allocaDeclarations, &variablesToDeclare, &sharedVariablesToDeclare, &shimFunctionsNeeded,
        &neededFunctions,
        &globalExpressionByValue, &localExpressionByValue);
    vector<string> extraInstructions;

    AllocaInst *alloca = new AllocaInst(IntegerType::get(context, 32));

    std::set< llvm::Function *> dumpedFunctions;
    map<Function *, Type *>returnTypeByFunction;
    instructionDumper.runRhsGeneration(alloca, &extraInstructions, dumpedFunctions, returnTypeByFunction);
    // cout << "last expression " << instructionDumper.lastExpression << endl;
    string expr = instructionDumper.localExpressionByValue->operator[](alloca);
    cout << "expr " << expr << endl;
}

}
