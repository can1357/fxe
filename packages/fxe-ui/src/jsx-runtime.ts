import type { ImageProps } from './components/Image.ts';
import type { PressableProps } from './components/Pressable.ts';
import type { ScrollViewProps } from './components/ScrollView.ts';
import type { TextProps } from './components/Text.ts';
import type { TextInputProps } from './components/TextInput.ts';
import type { ViewProps } from './components/View.ts';
import type { Node } from './index.ts';
import { Image, Pressable, ScrollView, Text, TextInput, View } from './index.ts';

export type JSXChild = Node | string | number | readonly JSXChild[] | null | undefined | boolean;
export type JSXNode = Node;

export const Fragment = Symbol.for('fxe.fragment');

type ComponentResult = Node;
type FunctionComponentType<P extends object = Record<string, unknown>> = (
  props: P,
) => ComponentResult;
type ClassComponentInstance = { render: () => ComponentResult; props?: unknown };
type ClassComponentType<P extends object = Record<string, unknown>> = new (
  props: P,
) => ClassComponentInstance;
type ComponentType<P extends object = Record<string, unknown>> =
  | FunctionComponentType<P>
  | ClassComponentType<P>;
type IntrinsicElementType = 'view' | 'text' | 'image' | 'pressable' | 'scroll' | 'input';
type ElementType<P extends object = Record<string, unknown>> =
  | IntrinsicElementType
  | typeof Fragment
  | ComponentType<P>;

type ElementProps = Record<string, unknown> & {
  children?: JSXChild;
  key?: string;
};

function mergedKey(props: ElementProps, key: string | undefined): string | undefined {
  return key ?? props.key;
}

function isNode(value: unknown): value is Node {
  if (typeof value !== 'object' || value === null) return false;
  const kind = (value as { type?: unknown }).type;
  return (
    kind === 'layer' ||
    kind === 'draw' ||
    kind === 'component' ||
    kind === 'provider' ||
    kind === 'portal' ||
    kind === 'error-boundary' ||
    kind === 'suspense'
  );
}

function isJSXChildArray(child: JSXChild): child is readonly JSXChild[] {
  return Array.isArray(child);
}

function normalizeChildren(child: JSXChild): Node[] {
  if (child === null || child === undefined || typeof child === 'boolean') return [];
  if (isJSXChildArray(child)) return child.flatMap((entry) => normalizeChildren(entry));
  if (isNode(child)) return [child];
  throw new TypeError('fxe-ui fragments must contain fxe-ui nodes');
}

function fragmentFromChildren(child: JSXChild): Node {
  return { type: 'layer', props: { children: normalizeChildren(child) } };
}

function isClassComponent(type: Function): boolean {
  const source = Function.prototype.toString.call(type);
  const prototype = type.prototype as { render?: unknown } | undefined;
  return /^class\s/.test(source) || typeof prototype?.render === 'function';
}

function renderClassComponent<P extends ElementProps>(
  type: ClassComponentType<P>,
  props: P,
): ComponentResult {
  const instance = new type(props);
  if (typeof instance.render !== 'function') {
    throw new TypeError('fxe-ui JSX class components must expose render()');
  }
  if (instance.props === undefined) instance.props = props;
  return instance.render();
}
function createElement<P extends ElementProps>(
  type: ElementType<P>,
  props: P | null,
  key?: string,
): JSXNode {
  const normalizedProps = props ?? ({} as P);
  const elementKey = mergedKey(normalizedProps, key);

  if (type === Fragment) return fragmentFromChildren(normalizedProps.children);

  if (typeof type === 'function') {
    const componentProps =
      elementKey === undefined ? normalizedProps : { ...normalizedProps, key: elementKey };
    if (isClassComponent(type)) {
      const classType = type as unknown as ClassComponentType<P>;
      return {
        type: 'component',
        componentType: type,
        render: (raw: unknown) => renderClassComponent(classType, raw as P),
        props: componentProps,
        displayName: type.name || 'JSXClassComponent',
        key: elementKey,
      };
    }
    const functionType = type as FunctionComponentType<P>;
    return {
      type: 'component',
      componentType: type,
      render: (raw: unknown) => functionType(raw as P),
      props: componentProps,
      displayName: type.name || 'JSXComponent',
      key: elementKey,
    };
  }

  if (type === 'view') return View({ ...(normalizedProps as never), key: elementKey });
  if (type === 'text') return Text({ ...(normalizedProps as never), key: elementKey });
  if (type === 'image') return Image({ ...(normalizedProps as never), key: elementKey });
  if (type === 'pressable') return Pressable({ ...(normalizedProps as never), key: elementKey });
  if (type === 'scroll') return ScrollView({ ...(normalizedProps as never), key: elementKey });
  if (type === 'input') return TextInput({ ...(normalizedProps as never), key: elementKey });

  const exhaustive: never = type;
  throw new TypeError(`Unsupported fxe-ui JSX element type: ${String(exhaustive)}`);
}

export function jsx<P extends ElementProps>(
  type: ElementType<P>,
  props: P | null,
  key?: string,
): JSXNode {
  return createElement(type, props, key);
}

export function jsxs<P extends ElementProps>(
  type: ElementType<P>,
  props: P | null,
  key?: string,
): JSXNode {
  return createElement(type, props, key);
}

export namespace JSX {
  export type Element = JSXNode;
  export interface ElementChildrenAttribute {
    children: {};
  }
  export interface IntrinsicElements {
    view: ViewProps;
    text: TextProps;
    image: ImageProps;
    pressable: PressableProps;
    scroll: ScrollViewProps;
    input: TextInputProps;
  }
}
