// A genuinely single-scalar output pointer (frexp's int *exp shape): written
// directly, never escapes to any other call or store. No array-bound
// evidence is found for it, but its usage is fully visible right here with
// no escape -- that absence, combined with full local visibility, is
// positive evidence of single-object access, so it must still be marshalled
// as one element, not force_localed by the escape gate.
double halve_and_report(double x, int *exp_out) {
  *exp_out = 42;
  return x / 2.0;
}
