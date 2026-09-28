"""Focused negative cases for the ownership inventory's borrow classifier."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "layers"))
from check_ownership import scan, apply_allowances


class OwnershipLifetimesTest(unittest.TestCase):
    def metrics(self, source, path="x.cpp"):
        return [item["metric"] for item in scan(source.encode(), path)]

    def test_lifetime_reference_capture_is_a_value_but_raw_this_still_fails(self):
        self.assertNotIn("unsafe_capture", self.metrics(
            "void f() { save([ref = lifetime_.ref(*this)] { use(ref); }); }"))
        self.assertIn("unsafe_capture", self.metrics(
            "void f() { save([ref = lifetime_.ref(*this), this] { use(this); }); }"))
        self.assertIn("unsafe_capture", self.metrics(
            "void f() { save([ptr = this] { use(ptr); }); }"))

    def test_handle_parameters_and_accessors_are_borrows_but_owners_fail(self):
        source = "void f(sqlite3* db, void* process_handle); sqlite3* get() const;"
        self.assertNotIn("raw_handle", self.metrics(source))
        source = "struct S { sqlite3* db = nullptr; void* process_handle = open(); };"
        self.assertEqual(2, self.metrics(source).count("raw_handle"))
        self.assertIn("raw_handle", self.metrics("void f() { sqlite3* db = open(); }"))

    def test_scope_exit_must_stay_in_its_local_scope(self):
        self.assertNotIn("unsafe_capture", self.metrics(
            "void f() { ScopeExit finish([this] { done(); }); finish.release(); }"))
        self.assertIn("unsafe_capture", self.metrics(
            "auto f() { ScopeExit finish([this] { done(); }); return std::move(finish); }"))

    def test_objective_c_arc_new_is_distinct_from_cpp_allocation(self):
        self.assertNotIn("raw_new", self.metrics("auto x = [Widget new];", "x.mm"))
        self.assertIn("raw_new", self.metrics("auto x = new Widget();", "x.mm"))

    def test_reviewed_sync_allowance_cannot_hide_an_async_capture(self):
        from types import SimpleNamespace
        rule = dict(kind="ownership_allow", path="x.cpp", rule="R15:unsafe_capture",
                    owner="audit", note="call=with; synchronous LifetimeRef borrow",
                    target="f", rank="1")
        policy = SimpleNamespace(rows=[rule], canonical=lambda path: path)
        found = scan(b"void f() { queue.enqueue([this] { use(); }); }", "x.cpp")
        remaining, allowed = apply_allowances(found, policy)
        self.assertEqual(1, len(remaining))
        self.assertFalse(allowed)

    def test_unknown_callback_api_is_not_assumed_synchronous(self):
        self.assertIn("unsafe_capture", self.metrics(
            "void f() { other.with([this] { use(); }); }"))


if __name__ == "__main__":
    unittest.main()
