"""CI stage, matrix and trigger assertions; no job-name-pattern dependency."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]

def read(path):
    return (ROOT / path).read_text()

class PipelineLayoutTests(unittest.TestCase):
    def test_stage_order(self):
        root = read(".gitlab-ci.yml")
        self.assertRegex(
            root, r"(?m)^stages:\s*\n  - preflight\n  - compile\n  - test\n  - hil\n  - merge-gate\n  - deploy"
        )
        self.assertNotIn("qspi-build.yml", root)

    def test_merge_request_metadata_is_enforced_after_qualification(self):
        root = read(".gitlab-ci.yml")
        self.assertLess(root.index("  - test\n"), root.index("  - merge-gate\n"))
        self.assertLess(root.index("  - hil\n"), root.index("  - merge-gate\n"))
        self.assertLess(root.index("  - merge-gate\n"), root.index("  - deploy\n"))
        governance = read(".gitlab/ci/governance.yml")
        self.assertIn("  stage: merge-gate", governance)
        self.assertNotIn("allow_failure:", governance)
        for job, script in (
            ("mr:release-notes", "check-mr-release-notes.sh"),
            ("mr:vnc-lib", "check-mr-vnc.sh"),
            ("mr:wifi-lib-revision", "check-wifi-lib-revision.sh"),
        ):
            self.assertIn(job + ":", governance)
            self.assertIn(script, governance)
            # Editable MR metadata remains in the late merge gate.
            block = governance.split(job + ":", 1)[1].split("\n\n", 1)[0]
            self.assertNotIn("  stage: preflight", block)
        # A missing DCO trailer cannot be repaired by editing MR metadata.
        commit_block = governance.split("mr:commit-policy:", 1)[1].split("\n\n", 1)[0]
        self.assertIn("  extends: .mr_governance", commit_block)
        self.assertIn("  stage: preflight", commit_block)
        self.assertIn("check-mr-commits.sh", commit_block)
        preflight = read(".gitlab/ci/validate.yml")
        self.assertIn("firmware-version:\n  stage: preflight", preflight)
        self.assertIn("auto-bump-version.sh", preflight)
        self.assertIn("mirrored-headers:\n  stage: preflight", preflight)

    def test_only_active_ci_configuration_is_included(self):
        root = read(".gitlab-ci.yml")
        self.assertNotIn("host-build.yml", root)
        validate = read(".gitlab/ci/validate.yml")
        self.assertIn("docs-check:\n  stage: preflight", validate)
        self.assertNotIn("generate_docs_html:", validate)
        self.assertNotIn("validate_docs_html:", validate)

    def test_ci_consumes_immutable_images_without_building_them(self):
        root = read(".gitlab-ci.yml")
        self.assertNotIn("tool-images.yml", root)
        self.assertFalse((ROOT / ".gitlab/ci/kernel-sdk").exists())
        self.assertFalse((ROOT / ".gitlab/ci/docker").exists())
        arm = read(".gitlab/ci/software-cross-arch.yml")
        manifest = read(".gitlab/ci/arm-images.yml")
        self.assertIn("image-recipe-sha256", arm)
        self.assertIn("verify_sdk.py", arm)
        self.assertIn("linux-build.sh", arm)
        self.assertIn("CI_CROSS_SDK_RECIPE_SHA256", manifest)
        self.assertEqual(manifest.count("ci/linux-cross-sdk@sha256:"), 5)
        framework = read(".gitlab/ci/software-framework.yml")
        self.assertIn("software:foundation:", framework)
        self.assertNotIn("LINUX_CI_TOOLBOX_IMAGE", framework)

    def test_preflight_is_lightweight(self):
        preflight = read(".gitlab/ci/validate.yml")
        for job in ("ci-syntax", "pre-commit", "docs-check"):
            self.assertIn(job + ":\n  stage: preflight", preflight)
        for job in ("pre-commit", "docs-check"):
            block = preflight.split("\n" + job + ":", 1)[1].split("\n\n", 1)[0]
            self.assertIn('image: "$ESP_CI_IMAGE"', block)
            self.assertNotIn("LINUX_CI_TOOLBOX_IMAGE == null", block)

    def test_kernel_and_arm_matrix_coverage(self):
        x86 = read(".gitlab/ci/linux.yml")
        arm = read(".gitlab/ci/software-cross-arch.yml")
        versions = re.findall(r'(?m)^    KERNEL_VERSION: "([\d.]+)"$', x86)
        self.assertEqual(len(versions), 12)
        self.assertEqual(len(set(versions)), 12)
        self.assertEqual(len(re.findall(r'(?m)^    PAGE_SIZE: "\d+"$', arm)), 5)
        for size in ("4096", "16384", "65536"):
            self.assertIn('PAGE_SIZE: "' + size + '"', arm)
        self.assertIn("module-audit-junit.xml", arm)

    def test_spi_hd_is_in_the_ordinary_firmware_matrix(self):
        fw = read(".gitlab/ci/firmware-build.yml")
        self.assertIn("stage: compile", fw)
        self.assertIn('TRANSPORT: ["spi_hd"]', fw)
        self.assertIn("allow_failure: true", fw)
        host = read(".gitlab/ci/linux.yml")
        self.assertIn("stage: compile", host)
        self.assertIn("stage: test", host)

    def test_test_only_edits_do_not_rebuild_all_compatibility_targets(self):
        for path, start, end in (
            (".gitlab/ci/linux.yml", ".linux_rules:", ".linux_shared:"),
            (".gitlab/ci/firmware-build.yml", ".firmware_build:", "firmware:"),
            (".gitlab/ci/software-cross-arch.yml", ".software_arm_compile:",
             "host:arm64-6.12-4k:"),
        ):
            block = read(path).split(start, 1)[1].split(end, 1)[0]
            self.assertNotIn("tests/software/**/*", block)

    def test_error_diagnostics_go_to_job_trace(self):
        runner = read(".gitlab/ci/software/run.py")
        self.assertIn("END FAILURE DIAGNOSTICS", runner)
        self.assertIn("junit.xml", runner)
        self.assertIn('tee "${log}"', read(".gitlab/ci/scripts/linux-quality.sh"))

if __name__ == "__main__":
    unittest.main()
