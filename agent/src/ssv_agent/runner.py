"""DeerFlowClient 单事件复核执行。"""

from __future__ import annotations

from typing import Any

from deerflow.client import DeerFlowClient

from ssv_agent.prompt import build_review_prompt
from ssv_agent.review_context import ReviewContext
from ssv_agent.review_context import RuleRetrievalContext


class ModelCallError(RuntimeError):
    """模型调用失败的稳定分类；``diagnostic`` 仅供内部日志使用。"""

    def __init__(self, code: str, message: str, diagnostic: str | None = None) -> None:
        super().__init__(message)
        self.code = code
        self.user_message = message
        self.diagnostic = diagnostic or message


_MODEL_ERROR_MESSAGES = {
    "model_insufficient_balance": "模型服务余额不足，请检查模型账户余额。",
    "model_authentication_failed": "模型服务认证失败，请检查模型凭据。",
    "model_access_denied": "模型服务拒绝访问，请检查模型权限。",
    "model_rate_limited": "模型服务请求频率受限，请稍后重试。",
    "model_timeout": "模型服务请求超时，请稍后重试。",
    "model_no_response": "模型服务未返回可解析结果。",
    "model_call_failed": "模型服务调用失败，请稍后重试或检查服务配置。",
}


def classify_model_error(exc: Exception) -> ModelCallError:
    """从上游异常文本提取稳定类别，但不把原文作为用户消息。"""
    text = str(exc)
    normalized = text.casefold()
    if "insufficient_balance" in normalized or "insufficient balance" in normalized or "余额不足" in text:
        code = "model_insufficient_balance"
    elif (
        "401" in normalized
        or "unauthorized" in normalized
        or "authentication" in normalized
        or "invalid api key" in normalized
    ):
        code = "model_authentication_failed"
    elif (
        "403" in normalized
        or "forbidden" in normalized
        or "access denied" in normalized
        or "permission denied" in normalized
    ):
        code = "model_access_denied"
    elif "429" in normalized or "rate limit" in normalized or "too many requests" in normalized or "throttl" in normalized:
        code = "model_rate_limited"
    elif "timeout" in normalized or "timed out" in normalized or "deadline exceeded" in normalized:
        code = "model_timeout"
    else:
        code = "model_call_failed"
    return ModelCallError(code, _MODEL_ERROR_MESSAGES[code], diagnostic=text)


def extract_final_answer(values_data: dict[str, Any]) -> str | None:
    """从 values 事件提取最终 AI 消息。"""
    messages = values_data.get("messages") or []
    for message in reversed(messages):
        if isinstance(message, dict) and message.get("type") == "ai":
            content = message.get("content") or ""
            if content:
                return content
    return None


def run_review(
    client: DeerFlowClient,
    context: ReviewContext,
    rule_context: RuleRetrievalContext | None = None,
) -> str:
    """执行一次复核，返回最终文本；异常向上抛出。"""
    thread_id = f"event-{context.event_id}"
    final_text = ""
    accumulated: list[str] = []

    try:
        for event in client.stream(
            thread_id=thread_id,
            message=build_review_prompt(context, rule_context),
        ):
            if event.type == "values":
                answer = extract_final_answer(event.data)
                if answer is not None:
                    final_text = answer
            elif event.type == "messages-tuple" and event.data.get("type") == "ai":
                accumulated.append(event.data.get("content") or "")
    except ModelCallError:
        raise
    except Exception as exc:
        raise classify_model_error(exc) from exc

    if not final_text:
        final_text = "".join(accumulated)
    final_text = final_text.strip()
    if not final_text:
        raise ModelCallError(
            "model_no_response",
            _MODEL_ERROR_MESSAGES["model_no_response"],
        )
    return final_text
