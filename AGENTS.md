# AGENTS.md

本文件是 Codex 在本仓库中的默认入口说明。

## 读取顺序

1. 先读 `CLAUDE.md`
2. 如果工作涉及 `pump/`，继续读 `pump/CLAUDE.md`
3. 如需细节，再按 `CLAUDE.md` / `pump/CLAUDE.md` 中的引用进入 `pump/ai_spec/*`

## 规则来源

- 仓库通用规则、目录说明、构建方式：以 `CLAUDE.md` 为准
- PUMP 框架规则、sender 语义、编码禁止项：以 `pump/CLAUDE.md` 及其引用文档为准
- 本文件只补充 Codex 侧的最小约定，不重复抄写 `CLAUDE.md`

## Codex 补充约定

- 处理 `apps/inconel/` 的设计或实现时，额外回看 `ai_context/inconel/known_issues.md`
- 如果当前功能所在分组里有“本步顺手做更合适”的相邻项，或下一步紧接着应该做的前置/后续项，必须主动提醒用户
- 如果某项更适合等另一个模块或前置功能落地后再做，也必须明确指出，不要等用户追问
