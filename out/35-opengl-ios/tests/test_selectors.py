# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 35 Part C1.3: host test of angle/objc_selectors.py's header parser (the CI runs it on the iOS
# SDK's headers) and of its check step, on small samples written like Apple's headers.
#   python3 -I tests/test_selectors.py

import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "angle"))
import objc_selectors as sel  # noqa: E402

SAMPLE = """
/* - (void)commented:(int)x; */
@protocol MTLCommandBuffer <NSObject>
@property (readonly) id <MTLDevice> device;
@property (copy, atomic) NSString *label;
@property (nonatomic, getter=isOpaque) BOOL opaque;
@property (nonatomic, setter=setFramebufferOnlyX:) BOOL framebufferOnly;
- (void)enqueue;
- (void)presentDrawable:(id <MTLDrawable>)drawable afterMinimumDuration:(CFTimeInterval)duration API_AVAILABLE(ios(10.3));
- (void)addCompletedHandler:(void (^)(id <MTLCommandBuffer>))block;
- (nullable id <MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:
        (MTLRenderPassDescriptor *)renderPassDescriptor;
+ (instancetype)renderPassDescriptor;
- (void)encodeWaitForEvent:(id <MTLEvent>)event value:(uint64_t)value // a comment
    API_AVAILABLE(macos(10.14), ios(12.0));
@end
static inline int notamethod(int a) { return a - 1; }
"""

checks = 0
failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("CHECK failed: %s" % what)


found = sel.header_selectors(SAMPLE)
for s in ("device", "label", "setLabel:", "isOpaque", "setOpaque:", "framebufferOnly",
          "setFramebufferOnlyX:", "enqueue", "presentDrawable:afterMinimumDuration:",
          "addCompletedHandler:", "renderCommandEncoderWithDescriptor:", "renderPassDescriptor",
          "encodeWaitForEvent:value:"):
    check(s in found, "declared: " + s)
for s in ("setDevice:", "commented:", "opaque", "setFramebufferOnly:", "notamethod"):
    check(s not in found, "not declared: " + s)

with tempfile.TemporaryDirectory() as d:
    with open(os.path.join(d, "sdk.txt"), "w") as fh:
        fh.write("\n".join(sorted(found)) + "\n")
    with open(os.path.join(d, "00001_a.methname"), "w") as fh:
        fh.write("Contents of (__TEXT,__objc_methname) section\n")
        fh.write("0000000000000000  enqueue\n0000000000000008  presentDrawable:afterMinimumDuration:\n")
        fh.write("0000000000000040  ownMethod:\n0000000000000050  privateThing\n")
    with open(os.path.join(d, "00001_a.ov"), "w") as fh:
        fh.write("    baseMethods 0x10\n\t\t  name 0x40 ownMethod:\n\t\t  types 0x60 v24@0:8@16\n")
    old = sys.stdout
    with open(os.path.join(d, "out.txt"), "w") as out:
        sys.stdout = out
        status = sel.check(os.path.join(d, "sdk.txt"), d)
    sys.stdout = old
    with open(os.path.join(d, "out.txt")) as fh:
        lines = fh.read().split("\n")
    check(status == 1, "check fails on an unknown selector")
    check("privateThing 00001_a" in lines, "the unknown selector is named with its member")
    check(not any(line.startswith("ownMethod:") for line in lines), "a member's own method is no finding")
    check(not any(line.startswith("enqueue") for line in lines), "a declared selector is no finding")

print("test_selectors: %d checks, %d failures" % (checks, failures))
sys.exit(1 if failures else 0)
