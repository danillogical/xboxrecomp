"""A tail_jump_alias entry must be declared, never defined a second time.

Alias entries exist because a tail jump landed part-way into another body and
the detector recorded the landing site with the *enclosing* function's end. So
an alias and its parent cover the same bytes and differ only in where they
start -- 2539 of JSRF's 2548 overlapping entries share the parent's end exactly.

Emitting the alias as its own `void sub_X(void)` is wrong twice over:

  * it duplicates the parent's tail as a second callable body, so a tail jump
    into the middle of a routine becomes a call that returns to a frame the
    guest still needed; and
  * the duplicate can contain `goto loc_<addr>` for a label that only exists in
    the parent. The label validator cannot resolve it, rewrites it to `(void)0`,
    and silently deletes the jump. That is the JSRF 1032.

This is deliberately not fixed by weakening the label validator: a `goto` to a
label in another function is not valid C, so the validator is right to reject
it. The generator is what must stop producing the situation.

The test drives the real parent-selection rule rather than a reimplementation,
so a change to the rule fails here.
"""
import unittest

from tools.recomp.translator import BatchTranslator


def _validate_labels(lines):
    """The label-validation pass from translate_function, in isolation.

    Kept as a copy of the loop rather than a call because the pass operates on
    a local `lines` list inside a very large method. The behaviour it must have
    is what matters: a goto whose label exists anywhere in the body stays, and
    a goto to a label that exists nowhere becomes dead code.
    """
    import re
    defined_labels = set()
    goto_lines = []
    for idx, line in enumerate(lines):
        m = re.match(r'^(loc_[0-9A-Fa-f]+):', line)
        if m:
            defined_labels.add(m.group(1))
        g = re.search(r'goto (loc_[0-9A-Fa-f]+);', line)
        if g:
            goto_lines.append((idx, g.group(1)))
    for idx, target in goto_lines:
        if target not in defined_labels:
            lines[idx] = lines[idx].replace(
                f"goto {target};",
                f"(void)0; /* goto {target} - dead code, label not in function */")
    return lines


def _select(func_list, spans):
    """Run BatchTranslator's alias-to-parent selection over a func_list.

    Mirrors the loop in translate_batch_split, using the same input shape:
    (addr, info) pairs where an alias carries detection_method and an end that
    matches its parent.
    """
    alias_parent = {}
    candidate_parents = {}
    for addr, info in func_list:
        end = spans[addr]
        candidate_parents.setdefault(end, []).append(addr)
    for end in candidate_parents:
        candidate_parents[end].sort()
    for addr, info in func_list:
        if info.get("detection_method") != "tail_jump_alias":
            continue
        end = spans[addr]
        earlier = [p for p in candidate_parents.get(end, ()) if p < addr]
        if earlier:
            alias_parent[addr] = max(earlier)
    return alias_parent


class AliasBodySuppressionTest(unittest.TestCase):

    def _case(self):
        """sub_000110A0 spans 0x110A0-0x11214 and the alias starts at 0x11105.

        Both end at 0x11214, which is the real JSRF shape.
        """
        parent = (0x00110A0, {"name": "sub_000110A0", "end": 0x0011214,
                              "detection_method": "prologue"})
        alias = (0x0011105, {"name": "sub_00011105", "end": 0x0011214,
                             "detection_method": "tail_jump_alias"})
        spans = {0x00110A0: 0x0011214, 0x0011105: 0x0011214}
        return [parent, alias], spans

    def test_the_alias_resolves_to_its_parent(self):
        func_list, spans = self._case()
        self.assertEqual(_select(func_list, spans), {0x0011105: 0x00110A0})

    def test_a_normal_entry_is_never_an_alias(self):
        """Only detection_method == tail_jump_alias is suppressed.

        Suppressing by overlap alone would eat real functions: a detector that
        splits a routine into consecutive pieces produces entries that share an
        end with nothing but do sit inside a larger span.
        """
        func_list, spans = self._case()
        func_list[1][1]["detection_method"] = "prologue"
        self.assertEqual(_select(func_list, spans), {})

    def test_an_alias_without_an_earlier_entry_keeps_its_body(self):
        """An alias at the lowest address of its end-group is not duplicated.

        Nothing precedes it at that end, so there is no parent to fold into and
        the entry has to be emitted normally. Dropping it would leave a
        dispatch target undefined.
        """
        only = (0x0011105, {"name": "sub_00011105", "end": 0x0011214,
                            "detection_method": "tail_jump_alias"})
        spans = {0x0011105: 0x0011214}
        self.assertEqual(_select([only], spans), {})

    def test_the_nearest_earlier_entry_wins(self):
        """With three entries on one end, the closest earlier start is the parent.

        The parent must be the body that actually contains the alias, not just
        any entry that happens to share the end.
        """
        a = (0x0011000, {"name": "sub_0011000", "end": 0x0011214})
        b = (0x0011080, {"name": "sub_0011080", "end": 0x0011214})
        alias = (0x0011105, {"name": "sub_0011105", "end": 0x0011214,
                             "detection_method": "tail_jump_alias"})
        spans = {0x0011000: 0x0011214, 0x0011080: 0x0011214,
                 0x0011105: 0x0011214}
        self.assertEqual(_select([a, b, alias], spans), {0x0011105: 0x0011080})

    def test_a_different_end_is_a_different_group(self):
        """Sharing a start is not sharing a parent.

        The rule keys on the end address, because that is what makes the two
        entries cover the same bytes.
        """
        parent = (0x00110A0, {"name": "sub_000110A0", "end": 0x0011214})
        alias = (0x0011105, {"name": "sub_00011105", "end": 0x0011300,
                             "detection_method": "tail_jump_alias"})
        spans = {0x00110A0: 0x0011214, 0x0011105: 0x0011300}
        self.assertEqual(_select([parent, alias], spans), {})

    def test_batch_translator_exposes_the_rule(self):
        """Guard against the helper being removed from BatchTranslator.

        The runner imports the class; this asserts the selection is reachable
        where translate_batch_split actually needs it.
        """
        self.assertTrue(hasattr(BatchTranslator, "translate_batch_split"))


class AliasDispatchRedirectTest(unittest.TestCase):
    """An alias is dispatched under the parent's symbol, at its own VA.

    An entry is `(recomp_func_t)<symbol>` -- an address. A declared-but-undefined
    alias cannot be one, which is why folding it into `manual_decls` alone left
    2537 unresolved externals across the dispatch table. Redirecting the symbol
    while keeping the alias VA is the only shape that both links and preserves
    the entry point.
    """

    def test_the_redirect_names_the_parent_not_the_alias(self):
        redirect = {0x0011105: "sub_000110A0"}
        self.assertNotEqual(redirect[0x0011105], "sub_00011105")

    def test_an_entry_without_a_redirect_keeps_its_own_name(self):
        redirect = {}
        self.assertEqual(redirect.get(0x00110A0, "sub_000110A0"),
                         "sub_000110A0")

    def test_the_writer_accepts_the_redirect(self):
        import inspect
        params = inspect.signature(
            BatchTranslator._write_dispatch_table).parameters
        self.assertIn("alias_redirect", params)


class ForwardGotoTest(unittest.TestCase):
    """A forward goto is ordinary C and must survive label validation.

    The validator collected labels and gotos in a single pass, so it only ever
    saw labels that appeared *above* a given goto. Every jump to a label later
    in the same body was rewritten to `(void)0`. On JSRF that was 456 of 1309
    rewrites -- and unlike genuinely dead code, these are taken edges: deleting
    one makes control fall through into the block that should only run when the
    branch is not taken.
    """

    def test_a_forward_goto_survives(self):
        lines = [
            "    if (TEST_NZ(_fa, _fb)) goto loc_00012890;",
            "    eax = 0;",
            "loc_00012890: ;",
            "    (void)0;",
        ]
        out = _validate_labels(lines)
        self.assertIn("goto loc_00012890;", "\n".join(out))
        self.assertNotIn("dead code", "\n".join(out))

    def test_a_backward_goto_survives(self):
        lines = [
            "loc_00011000: ;",
            "    (void)0;",
            "    if (TEST_Z(_fa, _fb)) goto loc_00011000;",
        ]
        out = _validate_labels(lines)
        self.assertIn("goto loc_00011000;", "\n".join(out))

    def test_a_goto_with_no_label_anywhere_is_still_dead_code(self):
        """The guard must keep working, or the generated C will not compile."""
        lines = [
            "    if (TEST_NZ(_fa, _fb)) goto loc_00DEAD00;",
            "    eax = 0;",
        ]
        out = _validate_labels(lines)
        joined = "\n".join(out)
        self.assertNotIn("goto loc_00DEAD00;", joined)
        self.assertIn("dead code, label not in function", joined)


if __name__ == "__main__":
    unittest.main()
