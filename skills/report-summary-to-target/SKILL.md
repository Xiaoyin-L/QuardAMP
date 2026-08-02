---
name: report-summary-to-target
description: Use when the user asks to summarize the preceding report, work log, implementation process, debugging session, or conversation into a target, especially phrases like "总结上面报告到xxx", "把上面的报告总结到xxx", "整理上述内容到xxx", or "生成进展报告到xxx". The summary must follow these sections: 已完成操作, 设计的关键步骤, 遇到的问题与调试解决, 最后的指标效果, 下一步操作.
metadata:
  short-description: Summarize prior work into a structured progress report
---

# Report Summary To Target

## Trigger

Use this skill when the user asks to summarize prior conversation or work into a destination such as a file, document, message, or chat reply.

Common Chinese triggers include:

- `总结上面报告到xxx`
- `把上面的报告总结到xxx`
- `整理上述内容到xxx`
- `生成进展报告到xxx`
- `把刚才的操作总结成报告`

## Scope

Treat `上面`, `上述`, `刚才`, or `前面的内容` as the relevant prior conversation, tool work, implementation notes, errors, fixes, test results, and user decisions from the current thread.

If there are multiple unrelated topics, summarize only the most recent coherent task unless the user names a broader scope.

Do not invent facts. If a section lacks evidence, write `暂无明确记录` or `未验证` instead of guessing.

## Target Handling

Resolve `xxx` from the user request:

- Local path or filename: create the file if missing. If it already exists, append a new dated report unless the user says to overwrite.
- Markdown target: write Markdown using the required section headings.
- Plain chat target, no target, or ambiguous target: provide the report directly in chat.
- Feishu/Lark/Doubao document URL or token: use the appropriate Lark document skill for that resource type, then write the report there.
- If the target cannot be resolved and writing externally is required, ask one concise clarification question.

## Required Output Structure

Use these exact top-level headings in this order:

```markdown
# 进展总结

## 已完成操作

## 设计的关键步骤

## 遇到的问题与调试解决

## 最后的指标效果

## 下一步操作
```

## Writing Rules

- Write in Chinese unless the user asks otherwise.
- Prefer concise bullet points under each heading.
- Include concrete filenames, commands, test names, URLs, metrics, and dates when they are present in the prior context.
- For `已完成操作`, focus on actions actually performed.
- For `设计的关键步骤`, explain the important implementation or reasoning decisions, not every tiny command.
- For `遇到的问题与调试解决`, pair each issue with its fix or current status.
- For `最后的指标效果`, include measured outcomes, test results, screenshots, benchmark numbers, deployment URLs, or validation status. If no metrics were collected, say so plainly.
- For `下一步操作`, list practical follow-ups in priority order.
- Keep the report useful as a handoff artifact: specific enough that another engineer can continue from it.

## Final Response After Writing

After writing to a target, briefly state where the report was written and mention whether it was appended or created. Do not paste the full report unless the user asks.
