// Two levels of struct nesting: fully resolvable at the built-in default
// max_type_depth (6), but exceeds a deliberately narrow max_type_depth=2
// (see CONFIG.md's analysis.max_type_depth), truncating the inner struct
// and forcing the whole function local.
struct Inner {
  int x;
};
struct Outer {
  struct Inner in;
};
void take_outer(struct Outer *o) {
  o->in.x = 1;
}
