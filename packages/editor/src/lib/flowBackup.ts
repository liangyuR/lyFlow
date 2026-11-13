import type { BaseEdit } from "../store/recipe";
import type { GraphDoc } from "../types/graph";
import type { RecipeSet } from "./recipes";

export interface FlowRecipeBackup {
  set: RecipeSet;
  saved: RecipeSet;
  current: string | null;
  currentId: string | null;
  dir: string | null;
  savedFiles: [string, string][];
  diskText: [string, string][];
  baseEdits?: Readonly<Record<string, BaseEdit>>;
}

export interface FlowTabBackup {
  id: string;
  title?: string;
  doc: GraphDoc;
  filePath: string | null;
  dirty: boolean;
  savedDoc?: GraphDoc | null;
  recipe: FlowRecipeBackup;
  recipeDirty: boolean;
}

export interface FlowWorkspaceBackup {
  version: 1;
  activeId: string;
  tabs: FlowTabBackup[];
}

export interface FlowBackupBackend {
  read(): Promise<unknown>;
  write(payload: FlowWorkspaceBackup): Promise<void>;
  clear(): Promise<void>;
}

const record = (value: unknown): value is Record<string, unknown> =>
  value !== null && typeof value === "object" && !Array.isArray(value);
const text = (value: unknown): value is string => typeof value === "string" && value.length > 0;
const nullableText = (value: unknown): value is string | null => value === null || typeof value === "string";

function graphShape(value: unknown): value is GraphDoc {
  if (!record(value) || !Number.isInteger(value.schemaVersion) || Number(value.schemaVersion) < 1 || !text(value.id)) return false;
  if (!Array.isArray(value.nodes) || !Array.isArray(value.edges)) return false;
  return value.nodes.every((node) => record(node) && text(node.id) && text(node.op)) &&
    value.edges.every((edge) => record(edge) && text(edge.id) &&
      record(edge.from) && text(edge.from.node) && text(edge.from.port) &&
      record(edge.to) && text(edge.to.node) && text(edge.to.port));
}

function recipeSetShape(value: unknown): value is RecipeSet {
  if (!record(value) || !Array.isArray(value.recipes) || !nullableText(value.defaultName)) return false;
  return value.recipes.every((recipe) => record(recipe) && text(recipe.id) && text(recipe.name) && record(recipe.values));
}

function entriesShape(value: unknown): value is [string, string][] {
  return Array.isArray(value) && value.every((entry) =>
    Array.isArray(entry) && entry.length === 2 && typeof entry[0] === "string" && typeof entry[1] === "string");
}

function recipeShape(value: unknown): value is FlowRecipeBackup {
  return record(value) && recipeSetShape(value.set) && recipeSetShape(value.saved) &&
    nullableText(value.current) && nullableText(value.currentId) && nullableText(value.dir) &&
    entriesShape(value.savedFiles) && entriesShape(value.diskText) &&
    (value.baseEdits === undefined || record(value.baseEdits));
}

export function isFlowWorkspaceBackup(value: unknown): value is FlowWorkspaceBackup {
  if (!record(value) || value.version !== 1 || !text(value.activeId) || !Array.isArray(value.tabs) || value.tabs.length === 0) return false;
  const ids = new Set<string>();
  for (const tab of value.tabs) {
    if (!record(tab) || !text(tab.id) || ids.has(tab.id) ||
      (tab.title !== undefined && typeof tab.title !== "string") ||
      !graphShape(tab.doc) || !nullableText(tab.filePath) || typeof tab.dirty !== "boolean" ||
      (tab.savedDoc !== undefined && tab.savedDoc !== null && !graphShape(tab.savedDoc)) ||
      !recipeShape(tab.recipe) || typeof tab.recipeDirty !== "boolean") return false;
    ids.add(tab.id);
  }
  return ids.has(value.activeId);
}

const DATABASE = "lyflow-workspace-backup";
const STORE = "workspace";
const KEY = "latest";

function openDatabase(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    if (typeof indexedDB === "undefined") {
      reject(new Error("IndexedDB 不可用"));
      return;
    }
    let settled = false;
    const request = indexedDB.open(DATABASE, 1);
    request.onupgradeneeded = () => {
      if (!request.result.objectStoreNames.contains(STORE)) request.result.createObjectStore(STORE);
    };
    request.onerror = () => {
      settled = true;
      reject(request.error ?? new Error("无法打开工作区备份"));
    };
    request.onblocked = () => {
      settled = true;
      reject(new Error("工作区备份被其他窗口占用"));
    };
    request.onsuccess = () => {
      if (settled) request.result.close();
      else resolve(request.result);
    };
  });
}

async function transact<T>(mode: IDBTransactionMode, operation: (store: IDBObjectStore) => IDBRequest<T>): Promise<T> {
  const database = await openDatabase();
  try {
    return await new Promise<T>((resolve, reject) => {
      const transaction = database.transaction(STORE, mode);
      const request = operation(transaction.objectStore(STORE));
      let result: T;
      request.onsuccess = () => { result = request.result; };
      request.onerror = () => reject(request.error ?? new Error("工作区备份读写失败"));
      transaction.oncomplete = () => resolve(result);
      transaction.onerror = () => reject(transaction.error ?? new Error("工作区备份读写失败"));
      transaction.onabort = () => reject(transaction.error ?? new Error("工作区备份事务已中止"));
    });
  } finally {
    database.close();
  }
}

const indexedDbBackend: FlowBackupBackend = {
  read: () => transact("readonly", (store) => store.get(KEY)),
  write: async (payload) => { await transact("readwrite", (store) => store.put(payload, KEY)); },
  clear: async () => { await transact("readwrite", (store) => store.delete(KEY)); },
};

const pending = new WeakMap<FlowBackupBackend, Promise<void>>();

function enqueue<T>(backend: FlowBackupBackend, operation: () => Promise<T>): Promise<T> {
  const next = (pending.get(backend) ?? Promise.resolve()).then(operation);
  pending.set(backend, next.then(() => {}, () => {}));
  return next;
}

export async function writeFlowBackup(payload: FlowWorkspaceBackup, backend: FlowBackupBackend = indexedDbBackend): Promise<boolean> {
  let snapshot: FlowWorkspaceBackup;
  try {
    if (!isFlowWorkspaceBackup(payload)) return false;
    snapshot = structuredClone(payload);
  } catch {
    return false;
  }
  return enqueue(backend, async () => {
    try {
      await backend.write(snapshot);
      return true;
    } catch {
      return false;
    }
  });
}

export function readFlowBackup(backend: FlowBackupBackend = indexedDbBackend): Promise<FlowWorkspaceBackup | null> {
  return enqueue(backend, async () => {
    try {
      const payload = await backend.read();
      return isFlowWorkspaceBackup(payload) ? structuredClone(payload) : null;
    } catch {
      return null;
    }
  });
}

export function clearFlowBackup(backend: FlowBackupBackend = indexedDbBackend): Promise<boolean> {
  return enqueue(backend, async () => {
    try {
      await backend.clear();
      return true;
    } catch {
      return false;
    }
  });
}
