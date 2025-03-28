
# docker build -t xahaud-hbb:latest build.hbb.dockerfile .
FROM ghcr.io/foobarwidget/holy-build-box-x64

# Set environment variables
ENV BUILD_CORES=8

# Create /io directory for mounting the working directory
RUN mkdir -p /io

# Copy dependency build script
COPY build-deps.sh /hbb/build-deps.sh
RUN chmod +x /hbb/build-deps.sh

# Run the dependency build script with proper error handling
RUN /hbb_exe/activate-exec bash -ex /hbb/build-deps.sh || (echo "ERROR: Dependency build failed" && exit 1)

# Set the entrypoint to activate the HBB environment and load our env settings
ENTRYPOINT ["/hbb_exe/activate-exec"]
CMD ["bash"]