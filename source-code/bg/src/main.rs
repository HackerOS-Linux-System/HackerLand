use smithay_client_toolkit::{
    data_device::DataDeviceHandler,
    default_environment,
    environment::{Environment, GlobalHandler, SimpleGlobal},
    output::OutputHandler,
    primary_selection::PrimarySelectionHandler,
    seat::SeatHandler,
    shm::ShmHandler,
};
use smithay_client_toolkit::reexports::client::{
    protocol::{wl_registry, wl_shm},
    Attached, DispatchData, Display,
};
use smithay_client_toolkit::reexports::protocols::wlr::unstable::layer_shell::v1::client::{
    zwlr_layer_shell_v1, zwlr_layer_surface_v1,
};

struct SimpleLayerShellHandler {
    shell: Option<Attached<zwlr_layer_shell_v1::ZwlrLayerShellV1>>,
}

impl SimpleLayerShellHandler {
    fn new() -> Self {
        SimpleLayerShellHandler { shell: None }
    }
}

// Implementacja GlobalHandler dla SimpleLayerShellHandler
impl GlobalHandler<zwlr_layer_shell_v1::ZwlrLayerShellV1> for SimpleLayerShellHandler {
    fn created(&mut self, registry: Attached<wl_registry::WlRegistry>, id: u32, version: u32, _: DispatchData) {
        let shell = registry.bind::<zwlr_layer_shell_v1::ZwlrLayerShellV1>(version, id);
        self.shell = Some((*shell).clone());
    }

    fn get(&self) -> Option<Attached<zwlr_layer_shell_v1::ZwlrLayerShellV1>> {
        self.shell.clone()
    }
}

// Makro default_environment generuje strukturę SimpleEnv.
// Definiujemy tylko własne pola, standardowe (compositor, shm, etc.) są dodawane automatycznie przez makro.
default_environment!(SimpleEnv,
    fields = [
        layer_shell: SimpleLayerShellHandler,
    ],
    singles = [
        zwlr_layer_shell_v1::ZwlrLayerShellV1 => layer_shell
    ],
);

fn main() {
    let display = Display::connect_to_env().expect("Failed to connect to Wayland server");
    let mut event_queue = display.create_event_queue();

    // Inicjalizacja komponentów zależnych.
    // SeatHandler musi być utworzony pierwszy, ponieważ DataDevice i PrimarySelection go wymagają.
    let mut seats = SeatHandler::new();
    let data_device_manager = DataDeviceHandler::init(&mut seats);
    let primary_selection_manager = PrimarySelectionHandler::init(&mut seats);

    // Inicjalizacja środowiska.
    // Należy użyć nazw pól generowanych przez makro (prefiks sctk_).
    // Dla Compositora i Subcompositora używamy SimpleGlobal, dla reszty odpowiednich Handlerów.
    let simple_env = SimpleEnv {
        layer_shell: SimpleLayerShellHandler::new(),
        sctk_compositor: SimpleGlobal::new(),
        sctk_subcompositor: SimpleGlobal::new(),
        sctk_shm: ShmHandler::new(),
        sctk_outputs: OutputHandler::new(),
        sctk_seats: seats,
        sctk_data_device_manager: data_device_manager,
        sctk_primary_selection_manager: primary_selection_manager,
    };

    let env = Environment::new(&display.attach(event_queue.token()), &mut event_queue, simple_env)
        .expect("Failed to create environment");

    let layer_shell = env.require_global::<zwlr_layer_shell_v1::ZwlrLayerShellV1>();

    for output in env.get_all_outputs() {
        let surface = env.create_surface();

        let layer_surface = layer_shell.get_layer_surface(
            &surface,
            Some(&output),
            zwlr_layer_shell_v1::Layer::Background,
            "hackerland-bg".to_string(),
        );

        layer_surface.set_size(0, 0);
        layer_surface.set_anchor(
            zwlr_layer_surface_v1::Anchor::Top |
            zwlr_layer_surface_v1::Anchor::Bottom |
            zwlr_layer_surface_v1::Anchor::Left |
            zwlr_layer_surface_v1::Anchor::Right
        );
        layer_surface.set_exclusive_zone(-1);

        surface.commit();

        let mut pool = env.create_auto_pool().expect("Failed to create shm pool");

        let width_i32 = 1920;
        let height_i32 = 1080;
        let stride_i32 = width_i32 * 4;

        let width_usize = width_i32 as usize;
        let height_usize = height_i32 as usize;
        let stride_usize = stride_i32 as usize;

        pool.resize(stride_usize * height_usize).expect("Failed to resize pool");

        // W SCTK 0.16 funkcja buffer zwraca krotkę (&mut [u8], WlBuffer).
        // Kolejność: (canvas, wl_buffer).
        let (canvas, wl_buffer) = pool.buffer(
            width_i32,
            height_i32,
            stride_i32,
            wl_shm::Format::Argb8888,
        ).expect("Failed to create buffer");

        // Rysowanie gradientu na canvasie (typu &mut [u8])
        for y in 0..height_usize {
            for x in 0..width_usize {
                let i = y * stride_usize + x * 4;

                let r = ((x as f32 / width_usize as f32) * 40.0 + 10.0) as u8;
                let g = ((y as f32 / height_usize as f32) * 30.0 + 10.0) as u8;
                let b = ((x as f32 / width_usize as f32) * 60.0 + 40.0) as u8;

                // BGRA (Little Endian ARGB)
                canvas[i] = b;
                canvas[i + 1] = g;
                canvas[i + 2] = r;
                canvas[i + 3] = 255;
            }
        }

        // surface.attach wymaga referencji do WlBuffer.
        surface.attach(Some(&wl_buffer), 0, 0);
        surface.damage_buffer(0, 0, width_i32, height_i32);
        surface.commit();
    }

    loop {
        event_queue.dispatch(&mut (), |_, _, _| {}).unwrap();
    }
}

