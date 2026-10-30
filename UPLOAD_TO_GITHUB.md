# Upload to GitHub (no git needed)
1. github.com → New repository → name it → Create (private is fine).
2. On the empty repo page click **uploading an existing file**. Drag in everything from this folder (open the folder, select all, drag the *contents*). Commit.
3. GitHub's drag-and-drop often skips hidden folders. Check that `.github/workflows/windows.yml` exists in the repo. If not:
   **Add file → Create new file**, type the name `.github/workflows/windows.yml` (typing the slashes makes the folders), paste the contents of `ci/windows.yml.copy`, commit.
4. Open the **Actions** tab → `windows-build` → **Run workflow**. When done, download the artifact `ASIOStreamBridge-win64`.
5. Follow `TEST_ON_WINDOWS.md`. If the build is red, copy the log text and send it back.
