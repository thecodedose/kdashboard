CREATE TABLE IF NOT EXISTS public.dashboard_notes (
  id text PRIMARY KEY CHECK (id = 'quick_note'),
  text text NOT NULL DEFAULT '' CHECK (octet_length(text) <= 16000),
  updated_at timestamptz NOT NULL DEFAULT now()
);

ALTER TABLE public.dashboard_notes ENABLE ROW LEVEL SECURITY;
-- Single-owner device data: only the server/admin connection may read or write.
REVOKE ALL ON public.dashboard_notes FROM anon, authenticated;
DROP TRIGGER IF EXISTS dashboard_notes_updated_at ON public.dashboard_notes;
CREATE TRIGGER dashboard_notes_updated_at BEFORE UPDATE ON public.dashboard_notes
  FOR EACH ROW EXECUTE FUNCTION system.update_updated_at();
INSERT INTO public.dashboard_notes (id, text) VALUES ('quick_note', '')
ON CONFLICT (id) DO NOTHING;
