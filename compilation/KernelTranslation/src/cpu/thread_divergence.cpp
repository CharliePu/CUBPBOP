// Modified from:
// https://github.com/taylorlloyd/GPUCheck.git
#include "thread_divergence.h"

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/CFG.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"

#include <unordered_map>
#include <queue>
#include <utility>

using namespace std;
using namespace llvm;


namespace ThreadDivergence
{
  class ThreadDivergenceAnalysis
  {
  public:
    bool isDependent(Value *v)
    {
      return taint[v];
    }

    bool runOnFunction(Function &F)
    {

      // Initialize ourselves
      for (auto v = F.arg_begin(), e = F.arg_end(); v != e; ++v)
        taint[&*v] = false; // Kernel parameters aren't tainted

      functionTainted(F, taint);

      for (auto b = F.begin(), e = F.end(); b != e; ++b)
      {
        for (auto i = b->begin(), e = b->end(); i != e; ++i)
        {
          errs() << (taint[&*i] ? "Thread-Dependent" : "Thread-Constant ") << " - ";
          i->dump();
          errs() << "\n";
        }
      }

      return false;
    }

    bool runOnModule(Module &M)
    {
      taint.clear();
      for (auto F = M.begin(), e = M.end(); F != e; ++F)
      {
        dbgs()<<"Function Name: "<<F->getName()<<"\n";
        dbgs()<<"Function Calling Conv: "<<F->getCallingConv()<<"\n";
        // Run directly over all kernels
        runOnFunction(*F);
      }

      // Merge all callsite taint
      for (auto ctaint = callTaint.begin(), e = callTaint.end(); ctaint != e; ++ctaint)
      {
        for (auto t = ctaint->second.begin(), e = ctaint->second.end(); t != e; ++t)
        {
          taint[t->first] |= t->second;
        }
      }
      return false;
    }

  private:
    unordered_map<Value *, bool> taint;
    unordered_map<CallInst *, unordered_map<Value *, bool>> callTaint;

    Value *getDominatingCondition(Instruction *left, Instruction *right, DominatorTree *DT)
    {
      return getDominatingCondition(left->getParent(), right->getParent(), DT);
    }

    Value *getDominatingCondition(BasicBlock *left, BasicBlock *right, DominatorTree *DT)
    {
      BasicBlock *dom = DT->findNearestCommonDominator(left, right);

      if (dom->size() == 0)
        return nullptr;
      auto last = --(dom->end());
      if (auto B = dyn_cast<BranchInst>(&*last))
      {
        if (B->isConditional())
          return B->getCondition();
      }
      return nullptr;
    }

    bool functionTainted(Function &F, unordered_map<Value *, bool> &taint)
    {
      queue<Value *> worklist;
      DominatorTree DT;
      DT.recalculate(F);

      // Everyone gets one look
      for (auto b = F.begin(), e = F.end(); b != e; ++b)
      {
        for (auto i = b->begin(), e = b->end(); i != e; ++i)
        {
          worklist.push(&*i);
        }
      }

      while (!worklist.empty())
      {
        Value *v = worklist.front();
        worklist.pop();
        update(v, isDependent(v, taint, &DT), taint, worklist);
      }

      // Collect all the return nodes
      vector<ReturnInst *> rets;
      for (auto b = F.begin(), e = F.end(); b != e; ++b)
      {
        for (auto i = b->begin(), e = b->end(); i != e; ++i)
        {
          if (auto ret = dyn_cast<ReturnInst>(&*i))
          {
            rets.push_back(ret);
          }
        }
      }

      // If any returns are directly tainted, return that
      for (auto ret = rets.begin(), e = rets.end(); ret != e; ++ret)
      {
        if (taint[*ret])
          return true;
      }

      // If the any return is on a tainted control-flow path, return that
      for (auto l = rets.begin(), e = rets.end(); l != e; ++l)
      {
        for (auto r = rets.begin(), e = rets.end(); r != e; ++r)
        {
          if (auto cond = getDominatingCondition(*l, *r, &DT))
          {
            if (taint[cond])
              return true;
          }
        }
      }

      return false;
    }

    void update(Value *v, bool newVal, unordered_map<Value *, bool> &taint, queue<Value *> &worklist)
    {
      bool oldVal = taint[v];
      taint[v] = newVal;

      if (newVal != oldVal)
      {
        errs() << "Update " << oldVal << "=>" << newVal << " for ";
        v->dump();
        errs() << "\n";
        // Update any users
        for (auto user = v->user_begin(), e = v->user_end(); user != e; ++user)
        {
          worklist.push(*user);
        }

        // Update any direct addresses
        if (auto S = dyn_cast<StoreInst>(v))
        {
          worklist.push(S->getPointerOperand());
        }
      }
    }

    bool isDependent(Value *v, unordered_map<Value *, bool> &taint, DominatorTree *DT)
    {
      // If this value uses any tainted values, it's tainted
      if (auto user = dyn_cast<User>(v))
      {
        for (auto op = user->op_begin(), e = user->op_end(); op != e; ++op)
        {
          if (taint[op->get()])
            return true;
        }
      }

      // If this value is the address of a tainted store, it's tainted
      for (auto u = v->use_begin(), e = v->use_end(); u != e; ++u)
      {
        if (auto S = dyn_cast<StoreInst>(u->getUser()))
        {
          if (taint[S])
          {
            return true;
          }
        }
      }

      // Special-case PHI Nodes
      if (auto PHI = dyn_cast<PHINode>(v))
      {
        // If the incoming path was selected on a control-flow dependent condition, then we're dependent
        for (auto l = PHI->block_begin(), e = PHI->block_end(); l != e; ++l)
        {
          for (auto r = PHI->block_begin(), e = PHI->block_end(); r != e; ++r)
          {
            if (auto C = getDominatingCondition(*l, *r, DT))
            {
              if (taint[C])
              {
                return true;
              }
            }
          }
        }
      }

      // Calls may directly reference the threadIdx
      if (auto CI = dyn_cast<CallInst>(v))
      {
        if (auto F = CI->getCalledFunction())
        {
          // is ThreadIdx Call
          if (F->getName().find("get_local_id") != F->getName().npos)
          {
            return true;
          }
          // Not an intrinsic, but a real function call
          if (!F->empty())
          {
            // Get the taint-map corresponding to this callsite
            unordered_map<Value *, bool> ctaint = callTaint[CI];
            auto arg = CI->arg_begin();

            // Propagate args to formals
            for (auto param = F->arg_begin(), e = F->arg_end(); param != e; ++param)
            {
              ctaint[&*param] = taint[arg->getUser()];
              ++arg;
            }

            // Solve for the called function
            return functionTainted(*F, ctaint);
          }
        }

        // Indirect call, abandon all hope here
      }

      return false;
    }

    bool isNvidiaKernel(Function &function)
    {
      auto state = function.getCallingConv() == CallingConv::PTX_Device ||
                   function.getCallingConv() == CallingConv::PTX_Kernel;
      if (state)
      {
        dbgs() << "isNvidiaKernel: kernel found \"" << function.getName() << "\"\n\n";
        return state;
      }
      return state;
    }
  };
}

void check_thread_divergence(Module *m) {
  ThreadDivergence::ThreadDivergenceAnalysis().runOnModule(*m);
}