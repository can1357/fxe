// Framework-internal prop keys used by the reconciler to propagate layout
// and inherited text style down through component boundaries.
//
// These are exported as `unique symbol`s rather than `__`-prefixed strings
// so they are invisible to `Object.keys` (and therefore to memo's default
// shallow-equal comparator). A user component wrapped in `memo(...)` will
// bail correctly even though every render carries fresh layout objects.

export const INTERNAL_LAYOUT: unique symbol = Symbol('fxe-ui.layout');
export const INTERNAL_TEXT_STYLE: unique symbol = Symbol('fxe-ui.textStyle');
