# Code Analysis Completeness Checklist

Before finalizing the analysis report, verify all applicable items:

## Scope & Planning
- [ ] Analysis scope is clearly defined (target, purpose, depth level)
- [ ] If incremental: Old report read and changes detected via `md5HashFile`
- [ ] If incremental: Only changed sections re-analyzed

## Codebase Understanding
- [ ] All entry points identified
- [ ] Directory structure mapped and categorized by layer
- [ ] Build configuration and external dependencies documented

## Architecture & Data Flow
- [ ] Architectural pattern identified
- [ ] Module dependencies mapped
- [ ] Key data flows traced (at least 2-3)
- [ ] Data models and storage layers documented

## Code Quality & Patterns
- [ ] Code patterns and conventions documented
- [ ] Complexity hotspots identified
- [ ] Duplication areas noted
- [ ] Technical debt cataloged

## Security & Performance
- [ ] Security posture assessed (auth, validation, secrets, vulnerabilities)
- [ ] Performance characteristics noted (bottlenecks, caching, concurrency)

## Infrastructure & Domain
- [ ] Deployment and infrastructure documented
- [ ] Domain and business logic analyzed
- [ ] Version control context reviewed (if git available)

## Extension & Risks
- [ ] Extension points identified
- [ ] Risks and technical debt cataloged

## Output & Delivery
- [ ] File integrity section updated with current MD5 hashes
- [ ] Output saved to `plan/code_analysis.md`
- [ ] Analysis loaded into RAG via `loadFileToRAG`
