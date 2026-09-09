# Windows isolated-repository execution rule

Codex thread completion is based on the requested source changes and source/static review. It is not conditioned on compilation, tests, hashes, GitHub state, or a commit.

Do not require or claim:

- compilation;
- host verification;
- SDK build verification;
- target verification;
- artifact or hash verification;
- GitHub remote, Actions, status, or submission verification;
- git commit, push, or pull request.

Source/static review is allowed.

Every thread completion report must end with:

```text
NOT COMPILED
NOT HOST VERIFIED
NOT SDK BUILD VERIFIED
NOT TARGET VERIFIED
```
