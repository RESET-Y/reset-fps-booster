// The newsletter edge function. Everything it does is described and done in
// handler.ts; this file only starts it, so the handler can be tested alone.
import { handler } from "./handler.ts";

Deno.serve(handler);
