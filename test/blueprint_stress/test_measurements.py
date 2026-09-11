"""Counterexamples for the benchmark's independent evidence joins."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from blueprint_stress import validate_measurements


def evidence(batch=1, failed=False):
    requests, reports = [], []
    for ident in range(1, batch + 1):
        main = ident == 1
        failure = main and failed
        matched = 3 if main else 0
        updated = 0 if failure else matched
        row = (f"BLUEPRINT_MEASURE_REQUEST: id={ident} microseconds=2 "
               f"matched={matched} updated={updated} outcome={0 if failure else 1} "
               f"points={9 if failure else 15} candidate_bytes={0 if failure else 100}")
        for point in ("entry", "compiled", "prepared", "retired"):
            used = 0 if failure and point in ("compiled", "prepared") else 200
            row += f" {point}_used={used} {point}_allocated={used + 20 if used else 0}"
        requests.append(row)
        prefix = "BLUEPRINT_STRESS_RESULT" if main else "BLUEPRINT_STRESS_AUX_RESULT"
        reports.append(f"{prefix}: id={ident} batch=1 status={'failed' if failure else 'completed'} matched={matched} updated={updated}")
    summary = f"BLUEPRINT_MEASURE_BATCH: requests={batch} microseconds={batch * 3} clock_ok=1 overflow=0"
    return "\n".join(requests + [summary] + reports)


class MeasurementJoinTests(unittest.TestCase):
    def check(self, text, *, batch=1, failed=False):
        case = {"count": 3, "cycles": 1, "batch": batch,
                "expected": "TARGET_LIMIT" if failed else "completed"}
        return validate_measurements(text, case)[1]

    def test_valid_completed_and_failed_requests(self):
        self.assertEqual([], self.check(evidence()))
        self.assertEqual([], self.check(evidence(failed=True), failed=True))

    def test_valid_four_request_batch(self):
        self.assertEqual([], self.check(evidence(batch=4), batch=4))

    def test_completed_lpc_result_cannot_claim_failed_metric(self):
        text = evidence().replace("outcome=1 points=15 candidate_bytes=100",
                                  "outcome=0 points=9 candidate_bytes=0")
        self.assertTrue(self.check(text))

    def test_missing_outcome_cannot_bypass_prepared_measurement(self):
        self.assertTrue(self.check(evidence().replace("outcome=1 ", "")))

    def test_success_requires_preparation_independent_of_metric_claim(self):
        self.assertTrue(self.check(evidence().replace("points=15", "points=11")))

    def test_request_ids_must_join(self):
        self.assertTrue(self.check(evidence().replace("REQUEST: id=1", "REQUEST: id=99")))

    def test_cohort_counts_must_join(self):
        self.assertTrue(self.check(evidence().replace("matched=3 updated=3 outcome=1",
                                                     "matched=4 updated=0 outcome=1")))

    def test_four_single_batches_are_not_one_four_request_batch(self):
        lines = evidence(batch=4).splitlines()
        split = []
        for line in lines[:4]:
            split.extend([line, "BLUEPRINT_MEASURE_BATCH: requests=1 microseconds=3 clock_ok=1 overflow=0"])
        split.extend(lines[5:])
        self.assertTrue(self.check("\n".join(split), batch=4))

    def test_mandatory_allocator_fields_cannot_be_missing(self):
        self.assertTrue(self.check(evidence().replace("entry_allocated=220", "")))

    def test_clock_failure_and_missing_batch_are_rejected(self):
        self.assertTrue(self.check(evidence().replace("clock_ok=1", "clock_ok=0")))
        self.assertTrue(self.check("\n".join(line for line in evidence().splitlines()
                                             if not line.startswith("BLUEPRINT_MEASURE_BATCH:"))))


if __name__ == "__main__":
    unittest.main()
