// callback_contract.c -- fixtures: typed function-pointer arguments,
// resolvable only via a reviewed callback contract (Config.h's
// CallbackRef/CallbackSignature), never by analyzing the callback's own
// body.

// The XERBLA-shaped error handler this schema is specifically shaped to
// cover: a bounded name buffer, a single scalar-pointer out-param, and
// a plain scalar length.
typedef void (*xerbla_handler_t)(const char *name, int *info, int name_length);

void takes_error_handler(xerbla_handler_t handler, double *x, int n) {
    if (handler)
        handler("takes_error_handler", &n, 19);
    for (int i = 0; i < n; i++)
        x[i] = 0.0;
}

// arg0 is an ordinary pointer, not a function pointer -- for the
// "target is not a function-pointer argument" rejection.
void plain_pointer_fn(double *x, int n) {
    for (int i = 0; i < n; i++)
        x[i] = 0.0;
}

// A 2-parameter (ptr, ptr) callback -- for the param-count and
// param-kind ABI-mismatch rejections.
typedef void (*two_param_handler_t)(const char *name, int *info);

void takes_two_param_handler(two_param_handler_t handler) {
    int info = 0;
    if (handler)
        handler("x", &info);
}

// A callback with a non-void (scalar) return -- for the return-shape
// ABI-mismatch rejection.
typedef int (*scalar_ret_handler_t)(int x);

void takes_scalar_ret_handler(scalar_ret_handler_t handler) {
    if (handler)
        handler(1);
}

// A struct carrying a function-pointer FIELD (a vtable-shaped idiom) --
// callback contracts are keyed by top-level argument index only, so a
// nested callback like this one is never resolvable, and must never be
// classified as a plain copyable blob of bytes: the field's real value
// is a function-table index with no meaning in another cage.
struct ops_with_callback {
    void (*callback)(int);
};

void takes_struct_with_callback(struct ops_with_callback *ops) {
    if (ops->callback)
        ops->callback(1);
}
