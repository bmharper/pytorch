#include <torch/csrc/distributed/rpc/torchscript_functions.h>

#include <torch/csrc/distributed/autograd/utils.h>
#include <torch/csrc/distributed/rpc/message.h>
#include <torch/csrc/distributed/rpc/rpc_agent.h>
#include <torch/csrc/distributed/rpc/script_call.h>
#include <torch/csrc/distributed/rpc/utils.h>

namespace torch {
namespace distributed {
namespace rpc {

c10::intrusive_ptr<c10::ivalue::Future> rpcTorchscript(
    const std::string& dstWorkerName,
    const c10::QualifiedName& qualifiedName,
    const c10::FunctionSchema& functionSchema,
    std::vector<c10::IValue>& stack) {
  auto scriptCall =
      std::make_unique<ScriptCall>(qualifiedName, std::move(stack));
  auto rpcAgentPtr = RpcAgent::getCurrentRpcAgent();
  auto futMessage = autograd::sendMessageWithAutograd(
      *rpcAgentPtr,
      rpcAgentPtr->getWorkerInfo(dstWorkerName),
      std::move(*scriptCall).toMessage());

  // Get function return type to construct c10::ivalue::Future.
  auto returns = functionSchema.returns();
  // Script call only allows single IValue returned.
  TORCH_INTERNAL_ASSERT(
      returns.size() == 1,
      "Return value of an annotated torchScript function should be a single "
      "IValue.",
      returns.size());
  auto returnType = returns.at(0).type();

  // Create a JIT future and pass it to futMessage's callback to set state
  // of the JIT future.
  auto futPtr = c10::make_intrusive<c10::ivalue::Future>(returnType);
  futMessage->addCallback([futPtr](
                              const rpc::Message& message,
                              const c10::optional<utils::FutureError>& futErr) {
    if (futErr) {
      c10::ivalue::Future::FutureError jitFutErr(std::string((*futErr).what()));
      futPtr->markCompleted(std::move(jitFutErr));
    } else {
      futPtr->markCompleted(deserializeRespToIValue(message));
    }
  });
  return futPtr;
}

std::shared_ptr<UserRRef> remoteTorchscript(
    const std::string& dstWorkerName,
    const c10::QualifiedName& qualifiedName,
    const c10::FunctionSchema& functionSchema,
    std::vector<c10::IValue>& stack) {
  auto rpcAgentPtr = RpcAgent::getCurrentRpcAgent();
  auto dstWorkerInfo = rpcAgentPtr->getWorkerInfo(dstWorkerName);
  auto& ctx = RRefContext::getInstance();
  // TODO: support creating RRefs on a local object.
  TORCH_INTERNAL_ASSERT(
      ctx.getWorkerId() != dstWorkerInfo.id_,
      "Does not support creating RRef on self yet.");

  // Get function return type to construct UserRRef.
  auto returns = functionSchema.returns();
  // Script call only allows single IValue returned.
  TORCH_INTERNAL_ASSERT(
      returns.size() == 1,
      "Return value of an annotated torchScript function should be a single "
      "IValue.",
      returns.size());
  auto returnType = returns.at(0).type();

  auto userRRefPtr = ctx.createUserRRef(dstWorkerInfo.id_, returnType);

  auto scriptRemoteCall = std::make_unique<ScriptRemoteCall>(
      qualifiedName,
      std::move(stack),
      userRRefPtr->rrefId(),
      userRRefPtr->forkId());

  auto fm = torch::distributed::autograd::sendMessageWithAutograd(
      *rpcAgentPtr,
      dstWorkerInfo,
      std::move(*scriptRemoteCall).toMessage(),
      false,
      nullptr);

  ctx.addPendingUser(userRRefPtr->forkId(), userRRefPtr);
  fm->addCallback(callback::confirmPendingUser);

  return userRRefPtr;
}

} // namespace rpc
} // namespace distributed
} // namespace torch
