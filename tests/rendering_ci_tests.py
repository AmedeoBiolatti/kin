"""Check the CI gate's failure behavior without needing a GPU or Vulkan layer."""

import importlib.util
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET


spec = importlib.util.spec_from_file_location(
    "rendering_gpu_check", Path(__file__).parents[1] / ".github/scripts/rendering-gpu-check.py"
)
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def report(*cases):
    root = ET.Element("testsuite")
    for name, output, status in cases:
        case = ET.SubElement(root, "testcase", name=name, status="run")
        if status:
            ET.SubElement(case, status)
        ET.SubElement(case, "system-out").text = output
    return ET.tostring(root, encoding="unicode")


class RenderingGateTests(unittest.TestCase):
    def test_plain_requires_every_test(self):
        xml = report(("a", "", None), ("b", "", None))
        errors, results = gate.check_report(xml, ("a", "b"))
        self.assertEqual(errors, [])
        self.assertEqual(len(results), 2)
        self.assertTrue(gate.check_report(xml, ("a", "b", "missing"))[0])

    def test_skips_fail_even_when_ctest_succeeds(self):
        for status in ("skipped", "failure", "error"):
            with self.subTest(status=status):
                self.assertTrue(gate.check_report(report(("a", "", status)), ("a",))[0])

    def test_empty_and_duplicate_reports_fail(self):
        self.assertTrue(gate.check_report(report(), ("a",))[0])
        self.assertTrue(gate.check_report(report(("a", "", None), ("a", "", None)), ("a",))[0])

    def test_validation_must_be_active_for_each_test(self):
        layer = 'INFO | LAYER: Insert instance layer "VK_LAYER_KHRONOS_validation" (/lib/layer.so)'
        self.assertEqual(gate.check_report(report(("a", layer, None)), ("a",), True)[0], [])
        self.assertTrue(gate.check_report(report(("a", layer, None), ("b", "", None)), ("a", "b"), True)[0])
        # Finding the manifest/library is insufficient evidence of layer insertion.
        self.assertTrue(gate.check_report(report(("a", "Loading layer library libVkLayer_khronos_validation.so", None)), ("a",), True)[0])

    def test_validation_errors_fail_passing_tests(self):
        for error in ("Validation Error: VUID-test", "SYNC-HAZARD-READ-AFTER-WRITE", "VK_ERROR_VALIDATION_FAILED_EXT"):
            with self.subTest(error=error):
                errors, results = gate.check_report(report(("a", error, None)), ("a",))
                self.assertTrue(errors)
                self.assertEqual(results[0]["validation_errors"], 1)

    def test_compatibility_warnings_are_preserved_without_failing(self):
        self.assertEqual(gate.check_report(report(("a", "Validation Warning: maintenance9", None)), ("a",))[0], [])


if __name__ == "__main__":
    unittest.main()
