---
sidebar_position: 10
---

import {gitHubRepo, gitHubRepoName, latestReleasePage} from '@site/constants'

import {Command, SLCommand} from '@site/elements'

import {latestReleaseVersion, macArmAsset, ubuntu20, ubuntu22, windowsAsset} from '@site/src/releaseData';

import CodeBlock from '@theme/CodeBlock';

# Installation

<p>The <a href={latestReleasePage}>latest release</a> is <code>{latestReleaseVersion}</code>.</p>

## Prebuilt binaries

### macOS

First, make sure that [Homebrew](https://brew.sh/) is installed on your system. Then either install directly from Homebrew-core or install the bottle released by us:

#### Installing from Homebrew-core

Just run:

```
brew install sapling
```

#### Installing from our prebuilt bottles

Follow the instructions depending on your architecture.

##### Apple silicon (arm64)

Download using `curl`:

<CodeBlock>
curl -L -o {macArmAsset.name} {macArmAsset.url}
</CodeBlock>

Then install:

<CodeBlock>
brew install ./{macArmAsset.name}
</CodeBlock>

:::caution

Downloading the bottle using a web browser instead of `curl` will cause macOS to tag Sapling as "untrusted" and the security manager will prevent you from running it. You can remove this annotation as follows:

<CodeBlock>
xattr -r -d com.apple.quarantine ~/Downloads/{macArmAsset.name}
</CodeBlock>

:::

Note that to clone larger repositories, you need to change the open files limit. We recommend doing it now so it doesn't bite you in the future:

```
echo "ulimit -n 1048576" >> ~/.bash_profile{'\n'}
echo "ulimit -n 1048576" >> ~/.zshrc
```

### Windows

After downloading the `sapling_windows` ZIP from the <a href={latestReleasePage}>latest release</a>, run the following in PowerShell as Administrator (substituting the name of the `.zip` file you downloaded, as appropriate):

<CodeBlock>
Expand-Archive ~/Downloads/{windowsAsset.name} 'C:\Program Files'{'\n'}
</CodeBlock>

This will create `C:\Program Files\Sapling`, which you likely want to add to your `%PATH%` environment variable using:

```
setx PATH "$env:PATH;C:\Program Files\Sapling" -m
```

Note the following tools must be installed to leverage Sapling's full feature set:

- [Git for Windows](https://git-scm.com/download/win) is required to use Sapling with Git repositories
- [Node.js](https://nodejs.org/en/download/) (v16 or later) is required to use <SLCommand name="web" />

Note that the name of the Sapling CLI `sl.exe` conflicts with the `sl` shell built-in in PowerShell (`sl` is an alias for `Set-Location`, which is equivalent to `cd`). If you want to use `sl` to run `sl.exe` in PowerShell, you must reassign the alias. Again, you must run the following as Administrator:

<CodeBlock>
Set-Alias -Name sl -Value 'C:\Program Files\Sapling\sl.exe' -Force -Option Constant,ReadOnly,AllScope{'\n'}
sl --version{'\n'}
Sapling {latestReleaseVersion}
</CodeBlock>

### Linux

#### Ubuntu 22.04

Download using `curl`:

<CodeBlock>
curl -L -o {ubuntu22.name} {ubuntu22.url}
</CodeBlock>

Then install:

<CodeBlock>
sudo apt install -y ./{ubuntu22.name}
</CodeBlock>

#### Arch Linux (AUR)

```
yay -S sapling-scm-bin
```

#### Other Linux distros

Sapling can be installed from Homebrew on Linux. First install Homebrew on your machine, then run

```
brew install sapling
```

## Building from source

In order to build from source, you need at least the following tools available in your environment:

- Make
- `g++`
- [Rust](https://www.rust-lang.org/tools/install)
- [Node.js](https://nodejs.org)
- [Yarn](https://yarnpkg.com/getting-started/install)

For the full list, find the appropriate `Dockerfile` for your platform that defines the image that is used for Sapling builds in automation to see which tools it installs. For example, <a href={`${gitHubRepo}/blob/main/.github/workflows/sapling-cli-ubuntu-22.04.Dockerfile`}><code>.github/workflows/sapling-cli-ubuntu-22.04.Dockerfile</code></a> reveals all of the packages you need to install via `apt-get` in the host environment in order to build Sapling from source.

Once you have your environment set up, you can do a build on macOS or Linux as follows:

<pre>{`\
git clone ${gitHubRepo}
cd ${gitHubRepoName}/eden/scm
make oss
./sl --help
`}
</pre>

To build on FreeBSD, you'll also need to setup a terminfo database and use GNU Make for the build:

<pre>{`\
pkg install gmake terminfo-db
export TERMINFO=/usr/local/share/terminfo
git clone ${gitHubRepo}
cd ${gitHubRepoName}/eden/scm
gmake oss
./sl --help
`}
</pre>

The Windows build has some additional dependencies and a separate build script. From the [GitHub Action used to build the Windows release](https://github.com/facebook/sapling/blob/main/.github/workflows/sapling-cli-windows-amd64-release.yml), perform the steps that use [`vcpkg`](https://vcpkg.io/) on your local machine to install the additional dependencies. Then you can build and run Sapling on Windows as follows:

<pre>{`\
git clone ${gitHubRepo}
cd ${gitHubRepoName}/eden/scm
git config --system core.longpaths true
python3 .\\packaging\\windows\\build_windows_zip.py
.\\build\\embedded\\sl.exe --help
`}
</pre>

Once you have Sapling installed, follow the [Getting Started](/docs/introduction/getting-started.md) instructions.
