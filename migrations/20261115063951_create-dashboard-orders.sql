CREATE TABLE IF NOT EXISTS public.dashboard_orders (
  order_key text PRIMARY KEY CHECK (length(order_key) BETWEEN 1 AND 160),
  store text NOT NULL CHECK (length(store) BETWEEN 1 AND 60),
  order_number text NOT NULL CHECK (length(order_number) BETWEEN 1 AND 80),
  items_summary text NOT NULL DEFAULT '' CHECK (length(items_summary) <= 380),
  status text NOT NULL CHECK (status IN ('placed','packed','shipped','out_for_delivery','partially_delivered','awaiting_update','exchange','delivered','cancelled','returned')),
  delivery_date date,
  delivery_window text NOT NULL DEFAULT '' CHECK (length(delivery_window) <= 32),
  courier text NOT NULL DEFAULT '' CHECK (length(courier) <= 48),
  tracking_number text NOT NULL DEFAULT '' CHECK (length(tracking_number) <= 48),
  tracking_url text NOT NULL DEFAULT '' CHECK (length(tracking_url) <= 2048 AND (tracking_url = '' OR tracking_url LIKE 'https://%')),
  delivery_otp text CHECK (delivery_otp ~ '^[0-9]{4,8}$'),
  otp_expires_at timestamptz,
  notes text NOT NULL DEFAULT '' CHECK (length(notes) <= 220),
  latest_email_at timestamptz NOT NULL,
  source_message_ids text[] NOT NULL CHECK (cardinality(source_message_ids) BETWEEN 1 AND 30),
  updated_at timestamptz NOT NULL DEFAULT now(),
  CHECK (delivery_otp IS NULL OR (status = 'out_for_delivery' AND otp_expires_at IS NOT NULL))
);
CREATE INDEX IF NOT EXISTS dashboard_orders_latest_email_idx ON public.dashboard_orders (latest_email_at DESC);
CREATE TABLE IF NOT EXISTS public.dashboard_order_sync (
  id text PRIMARY KEY CHECK (id = 'gmail'),
  checked_at timestamptz NOT NULL,
  interval_hours integer NOT NULL DEFAULT 4 CHECK (interval_hours = 4)
);
ALTER TABLE public.dashboard_orders ENABLE ROW LEVEL SECURITY;
ALTER TABLE public.dashboard_order_sync ENABLE ROW LEVEL SECURITY;
REVOKE ALL ON public.dashboard_orders, public.dashboard_order_sync FROM PUBLIC, anon, authenticated;
CREATE OR REPLACE FUNCTION public.dashboard_orders_clear_closed_otp() RETURNS trigger LANGUAGE plpgsql SET search_path = pg_catalog, public AS $$
BEGIN
  IF NEW.status <> 'out_for_delivery' THEN NEW.delivery_otp := NULL; NEW.otp_expires_at := NULL; END IF;
  NEW.updated_at := now();
  RETURN NEW;
END;
$$;
DROP TRIGGER IF EXISTS dashboard_orders_before_write ON public.dashboard_orders;
CREATE TRIGGER dashboard_orders_before_write BEFORE INSERT OR UPDATE ON public.dashboard_orders FOR EACH ROW EXECUTE FUNCTION public.dashboard_orders_clear_closed_otp();
REVOKE ALL ON FUNCTION public.dashboard_orders_clear_closed_otp() FROM PUBLIC, anon, authenticated;
