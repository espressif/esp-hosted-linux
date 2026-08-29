ARG BASE_IMAGE
FROM ${BASE_IMAGE}

ARG KERNEL_VERSION

COPY sdk/ /kernel-sdk/${KERNEL_VERSION}/

RUN test -f "/kernel-sdk/${KERNEL_VERSION}/Makefile" && \
    test -s "/kernel-sdk/${KERNEL_VERSION}/.config" && \
    test -s "/kernel-sdk/${KERNEL_VERSION}/Module.symvers" && \
    test "$(make -s -C "/kernel-sdk/${KERNEL_VERSION}" kernelversion)" = "${KERNEL_VERSION}"

ENV KERNEL_VERSION=${KERNEL_VERSION}
ENV KERNEL=/kernel-sdk/${KERNEL_VERSION}

CMD ["/bin/bash"]
