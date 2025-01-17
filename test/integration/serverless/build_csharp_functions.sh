#!/bin/bash

echo "Building C# Lambda functions for $ARCHITECTURE architecture"
cd src/csharp-tests
dotnet restore --verbosity quiet
set +e #set this so we don't exit if the tools are already installed
dotnet tool install -g Amazon.Lambda.Tools --framework netcoreapp3.1 --verbosity quiet
set -e

if [ $ARCHITECTURE == "arm64" ]; then
    CONVERTED_ARCH=$ARCHITECTURE
else
    # dotnet package function uses x86_64 instead of amd64
    CONVERTED_ARCH="x86_64"
fi

dotnet lambda package --configuration Release --framework netcoreapp3.1 --verbosity quiet --output-package bin/Release/netcoreapp3.1/handler.zip --function-architecture $CONVERTED_ARCH