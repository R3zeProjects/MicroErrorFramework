#!/usr/bin/env bash

set -euo pipefail

# GitHub-hosted Ubuntu runners normally use an Azure mirror. During regional
# outages apt can spend several minutes retrying it before falling back. Use the
# canonical HTTPS mirror and retain apt's own bounded retry policy.
for source_file in /etc/apt/sources.list /etc/apt/sources.list.d/ubuntu.sources; do
    if [[ -f "${source_file}" ]]; then
        sudo sed -i \
            's|http://azure.archive.ubuntu.com/ubuntu|https://archive.ubuntu.com/ubuntu|g' \
            "${source_file}"
    fi
done

apt_options=(
    -o Acquire::Retries=5
    -o Acquire::http::Timeout=30
    -o Acquire::https::Timeout=30
)

sudo apt-get "${apt_options[@]}" update
sudo DEBIAN_FRONTEND=noninteractive apt-get "${apt_options[@]}" install \
    --yes --no-install-recommends "$@"
